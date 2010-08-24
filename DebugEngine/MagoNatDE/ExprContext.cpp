/*
   Copyright (c) 2010 Aldo J. Nunez

   Licensed under the Apache License, Version 2.0.
   See the LICENSE text file for details.
*/

#include "Common.h"
#include "ExprContext.h"
#include "Expr.h"
#include "Module.h"
#include "Thread.h"
#include "CVDecls.h"
#include "DebuggerProxy.h"
#include "RegisterSet.h"
#include "winternl2.h"
#include <MagoCVConst.h>

using namespace MagoST;


// from DMD source
#define DMD_OEM 0x42    // Digital Mars OEM number (picked at random)
#define DMD_OEM_DARRAY      1
#define DMD_OEM_AARRAY      2
#define DMD_OEM_DELEGATE    3


namespace Mago
{
    // ExprContext

    ExprContext::ExprContext()
        :   mPC( 0 )
    {
        memset( &mFuncSH, 0, sizeof mFuncSH );
        memset( &mBlockSH, 0, sizeof mBlockSH );
    }

    ExprContext::~ExprContext()
    {
    }


    ////////////////////////////////////////////////////////////////////////////// 
    // IDebugExpressionContext2 

    HRESULT ExprContext::GetName( BSTR* pbstrName ) 
    {
        return E_NOTIMPL;
    }

    HRESULT ExprContext::ParseText( 
            LPCOLESTR pszCode,
            PARSEFLAGS dwFlags,
            UINT nRadix,
            IDebugExpression2** ppExpr,
            BSTR* pbstrError,
            UINT* pichError )
    {
        OutputDebugStringA( "ExprContext::ParseText\n" );

        HRESULT hr = S_OK;
        RefPtr<Expr>    expr;
        RefPtr<MagoEE::IEEDParsedExpr>  parsedExpr;

        hr = MakeCComObject( expr );
        if ( FAILED( hr ) )
            return hr;

        hr = MagoEE::EED::ParseText( pszCode, mTypeEnv, mStrTable, parsedExpr.Ref() );
        if ( FAILED( hr ) )
        {
            MagoEE::EED::GetErrorString( hr, *pbstrError );
            return hr;
        }

        MagoEE::EvalOptions options = { 0 };

        hr = parsedExpr->Bind( options, this );
        if ( FAILED( hr ) )
        {
            MagoEE::EED::GetErrorString( hr, *pbstrError );
            return hr;
        }

        hr = expr->Init( parsedExpr, pszCode, this );
        if ( FAILED( hr ) )
            return hr;

        *ppExpr = expr.Detach();
        return S_OK;
    }


    ////////////////////////////////////////////////////////////////////////////// 
    // MagoEE::IValueBinder

    HRESULT ExprContext::FindObject( 
        const wchar_t* name, 
        MagoEE::Declaration*& decl )
    {
        HRESULT hr = S_OK;
        CAutoVectorPtr<char>        u8Name;
        size_t                      u8NameLen = 0;
        MagoST::SymHandle           symHandle = { 0 };
        RefPtr<MagoST::ISession>    session;

        if ( !mModule->GetSymbolSession( session ) )
            return E_NOT_FOUND;

        hr = Utf16To8( name, wcslen( name ), u8Name.m_p, u8NameLen );
        if ( FAILED( hr ) )
            return hr;

        hr = FindLocalSymbol( u8Name, u8NameLen, symHandle );
        if ( hr != S_OK )
        {
            hr = FindGlobalSymbol( u8Name, u8NameLen, symHandle );
            if ( hr != S_OK )
                return E_NOT_FOUND;
        }

        hr = MakeDeclarationFromSymbol( symHandle, decl );
        if ( FAILED( hr ) )
            return hr;

        return S_OK;
    }

    HRESULT ExprContext::GetThis( MagoEE::Declaration*& decl )
    {
        HRESULT hr = S_OK;
        MagoST::SymHandle           childSH;
        RefPtr<MagoST::ISession>    session;

        if ( !mModule->GetSymbolSession( session ) )
            return E_NOT_FOUND;

        hr = session->FindChildSymbol( mBlockSH, "this", 4, childSH );
        if ( hr != S_OK )
            return E_NOT_FOUND;

        hr = MakeDeclarationFromSymbol( childSH, decl );
        if ( FAILED( hr ) )
            return hr;

        return S_OK;
    }

    HRESULT ExprContext::GetSuper( MagoEE::Declaration*& decl )
    {
        return E_NOTIMPL;
    }

    HRESULT ExprContext::GetReturnType( MagoEE::Type*& type )
    {
        return E_NOTIMPL;
    }

    HRESULT ExprContext::GetAddress( MagoEE::Declaration* decl, MagoEE::Address& addr )
    {
        if ( decl == NULL )
            return E_INVALIDARG;

        HRESULT hr = S_OK;
        CVDecl*                     cvDecl = (CVDecl*) decl;
        MagoST::ISymbolInfo*        sym = cvDecl->GetSymbol();
        MagoST::LocationType        loc = MagoST::LocIsNull;
        MagoEE::DataValueKind       valKind = MagoEE::DataValueKind_None;
        MagoEE::DataValue           dataVal = { 0 };
        RefPtr<MagoST::ISession>    session;

        if ( !mModule->GetSymbolSession( session ) )
            return E_NOT_FOUND;

        if ( !sym->GetLocation( loc ) )
            return E_FAIL;

        switch ( loc )
        {
        case LocIsRegRel:
            {
                uint32_t    reg = 0;
                int32_t     offset = 0;

                if ( !sym->GetRegister( reg ) )
                    return E_FAIL;

                if ( !sym->GetOffset( offset ) )
                    return E_FAIL;

                hr = GetRegValue( reg, valKind, dataVal );
                if ( FAILED( hr ) )
                    return E_FAIL;

                if ( (valKind != MagoEE::DataValueKind_Int64) && (valKind != MagoEE::DataValueKind_UInt64) )
                    return E_FAIL;

                addr = dataVal.UInt64Value + offset;
            }
            break;

        case LocIsStatic:
            {
                uint16_t        sec = 0;
                uint32_t        offset = 0;

                if ( !sym->GetAddressOffset( offset ) )
                    return E_FAIL;
                if ( !sym->GetAddressSegment( sec ) )
                    return E_FAIL;

                addr = session->GetVAFromSecOffset( sec, offset );
                if ( addr == 0 )
                    return E_FAIL;
            }
            break;

        case LocIsTLS:
            {
                uint32_t        offset = 0;
                DWORD           tebAddr = 0;
                DWORD           tlsArrayPtrAddr = 0;
                DWORD           tlsArrayAddr = 0;
                DWORD           tlsBufAddr = 0;
                DebuggerProxy*  debuggerProxy = mThread->GetDebuggerProxy();

                if ( !sym->GetAddressOffset( offset ) )
                    return E_FAIL;

                // TODO: rename to GetTebBase
                tebAddr = mThread->GetCoreThread()->GetTlsBase();

                tlsArrayPtrAddr = tebAddr + offsetof( TEB32, ThreadLocalStoragePointer );

                SIZE_T  lenRead = 0;
                SIZE_T  lenUnreadable = 0;

                hr = debuggerProxy->ReadMemory( 
                    mThread->GetCoreProcess(), 
                    tlsArrayPtrAddr, 
                    4, 
                    lenRead, 
                    lenUnreadable, 
                    (uint8_t*) &tlsArrayAddr );
                if ( FAILED( hr ) )
                    return hr;
                if ( lenRead < 4 )
                    return E_FAIL;

                if ( (tlsArrayAddr == 0) || (tlsArrayAddr == tlsArrayPtrAddr) )
                {
                    // go to the reserved slots in the TEB
                    tlsArrayAddr = tebAddr + offsetof( TEB32, TlsSlots );
                }

                // assuming TLS slot 0
                hr = debuggerProxy->ReadMemory( 
                    mThread->GetCoreProcess(), 
                    tlsArrayAddr, 
                    4, 
                    lenRead, 
                    lenUnreadable, 
                    (uint8_t*) &tlsBufAddr );
                if ( FAILED( hr ) )
                    return hr;
                if ( lenRead < 4 )
                    return E_FAIL;

                addr = tlsBufAddr + offset;
            }
            break;

        default:
            return E_FAIL;
        }

        return S_OK;
    }

    HRESULT ExprContext::GetValue( 
        MagoEE::Declaration* decl, 
        MagoEE::DataValue& value )
    {
        if ( decl == NULL )
            return E_INVALIDARG;

        HRESULT hr = S_OK;
        CVDecl*                     cvDecl = (CVDecl*) decl;
        MagoST::ISymbolInfo*        sym = cvDecl->GetSymbol();
        MagoST::LocationType        loc = MagoST::LocIsNull;
        MagoEE::DataValueKind       valKind = MagoEE::DataValueKind_None;
        RefPtr<MagoEE::Type>        type;

        if ( !decl->GetType( type.Ref() ) )
            return E_FAIL;

        if ( !sym->GetLocation( loc ) )
            return E_FAIL;

        switch ( loc )
        {
        case LocIsRegRel:
        case LocIsStatic:
        case LocIsTLS:
            {
                MagoEE::Address addr = 0;

                hr = GetAddress( decl, addr );
                if ( FAILED( hr ) )
                    return hr;

                return GetValue( addr, type, value );
            }
            break;

        case LocIsConstant:
            {
                MagoST::Variant var = { 0 };

                if ( !sym->GetValue( var ) )
                    return E_FAIL;

                ConvertVariantToDataVal( var, value );
            }
            break;

        case LocIsEnregistered:
            {
                uint32_t    reg = 0;

                if ( !sym->GetRegister( reg ) )
                    return E_FAIL;

                return GetRegValue( reg, valKind, value );
            }
            break;

        // GetValue is not the right way to get a LocIsThisRel, and LocIsBitField isn't supported
        default:
            return E_FAIL;
        }

        return S_OK;
    }

    HRESULT ExprContext::GetValue( 
        MagoEE::Address addr, 
        MagoEE::Type* type, 
        MagoEE::DataValue& value )
    {
        HRESULT         hr = S_OK;
        uint8_t         targetBuf[ sizeof( MagoEE::DataValue ) ] = { 0 };
        size_t          targetSize = type->GetSize();
        SIZE_T          lenRead = 0;
        SIZE_T          lenUnreadable = 0;
        DebuggerProxy*  debuggerProxy = mThread->GetDebuggerProxy();

        // no value to get for complex/aggregate types
        if ( !type->IsScalar() 
            && !type->IsDArray() 
            && !type->IsAArray() 
            && !type->IsDelegate() )
            return S_OK;

        if ( targetSize > sizeof targetBuf )
            targetSize = sizeof targetBuf;

        hr = debuggerProxy->ReadMemory( 
            mThread->GetCoreProcess(), 
            (Address) addr, 
            targetSize, 
            lenRead, 
            lenUnreadable, 
            targetBuf );
        if ( FAILED( hr ) )
            return hr;
        if ( lenRead < targetSize )
            return HRESULT_FROM_WIN32( ERROR_PARTIAL_COPY );

        if ( type->IsPointer() )
        {
            value.Addr = ReadInt( targetBuf, 0, targetSize, false );
        }
        else if ( type->IsIntegral() )
        {
            value.UInt64Value = ReadInt( targetBuf, 0, targetSize, type->IsSigned() );
        }
        else if ( type->IsReal() || type->IsImaginary() )
        {
            value.Float80Value = ReadFloat( targetBuf, 0, type );
        }
        else if ( type->IsComplex() )
        {
            const size_t    PartSize = type->GetSize() / 2;

            value.Complex80Value.RealPart = ReadFloat( targetBuf, 0, type );
            value.Complex80Value.ImaginaryPart = ReadFloat( targetBuf, PartSize, type );
        }
        else if ( type->IsDArray() )
        {
            const size_t LengthSize = type->AsTypeDArray()->GetLengthType()->GetSize();
            const size_t AddressSize = type->AsTypeDArray()->GetPointerType()->GetSize();

            value.Array.Length = (MagoEE::dlength_t) ReadInt( targetBuf, 0, LengthSize, false );
            value.Array.Addr = ReadInt( targetBuf, LengthSize, AddressSize, false );
        }
        else if ( type->IsAArray() )
        {
            value.Addr = ReadInt( targetBuf, 0, targetSize, false );
        }
        else if ( type->IsDelegate() )
        {
            const size_t AddressSize = mTypeEnv->GetVoidPointerType()->GetSize();

            value.Delegate.ContextAddr = (MagoEE::dlength_t) ReadInt( targetBuf, 0, AddressSize, false );
            value.Delegate.FuncAddr = ReadInt( targetBuf, AddressSize, AddressSize, false );
        }
        else
            return E_FAIL;

        return S_OK;
    }

    HRESULT ExprContext::SetValue( 
        MagoEE::Declaration* decl, 
        const MagoEE::DataValue& value )
    {
        if ( decl == NULL )
            return E_INVALIDARG;

        HRESULT hr = S_OK;
        CVDecl*                     cvDecl = (CVDecl*) decl;
        MagoST::ISymbolInfo*        sym = cvDecl->GetSymbol();
        MagoST::LocationType        loc = MagoST::LocIsNull;
        RefPtr<MagoEE::Type>        type;

        if ( !decl->GetType( type.Ref() ) )
            return E_FAIL;

        if ( !sym->GetLocation( loc ) )
            return E_FAIL;

        switch ( loc )
        {
        case LocIsRegRel:
        case LocIsStatic:
        case LocIsTLS:
            {
                MagoEE::Address addr = 0;

                hr = GetAddress( decl, addr );
                if ( FAILED( hr ) )
                    return hr;

                return SetValue( addr, type, value );
            }
            break;

        case LocIsEnregistered:
            {
                uint32_t    reg = 0;

                if ( !sym->GetRegister( reg ) )
                    return E_FAIL;

                // TODO: writing to registers
                return E_NOTIMPL;
            }
            break;

        // SetValue is not the right way to get a LocIsThisRel, and LocIsBitField isn't supported
        // can't write to constants either
        default:
            return E_FAIL;
        }

        return S_OK;
    }

    HRESULT ExprContext::SetValue( 
        MagoEE::Address addr, 
        MagoEE::Type* type, 
        const MagoEE::DataValue& value )
    {
        HRESULT         hr = S_OK;
        uint8_t         sourceBuf[ sizeof( MagoEE::DataValue ) ] = { 0 };
        size_t          sourceSize = type->GetSize();
        SIZE_T          lenWritten = 0;
        DebuggerProxy*  debuggerProxy = mThread->GetDebuggerProxy();

        // no value to set for complex/aggregate types
        if ( !type->IsScalar() 
            && !type->IsDArray() 
            && !type->IsAArray() 
            && !type->IsDelegate() )
            return S_OK;

        if ( sourceSize > sizeof sourceBuf )
            return E_FAIL;

        if ( type->IsPointer() )
        {
            hr = WriteInt( sourceBuf, sizeof sourceBuf, type, value.Addr );
        }
        else if ( type->IsIntegral() )
        {
            hr = WriteInt( sourceBuf, sizeof sourceBuf, type, value.UInt64Value );
        }
        else if ( type->IsReal() || type->IsImaginary() )
        {
            hr = WriteFloat( sourceBuf, sizeof sourceBuf, type, value.Float80Value );
        }
        else if ( type->IsComplex() )
        {
            const size_t    PartSize = type->GetSize() / 2;

            hr = WriteFloat( sourceBuf, sizeof sourceBuf, type, value.Complex80Value.RealPart );
            if ( FAILED( hr ) )
                return hr;

            hr = WriteFloat( 
                sourceBuf + PartSize, 
                sizeof sourceBuf - PartSize, 
                type, 
                value.Complex80Value.ImaginaryPart );
        }
        else if ( type->IsDArray() )
        {
            MagoEE::ITypeDArray* arrayType = type->AsTypeDArray();
            const size_t LengthSize = arrayType->GetLengthType()->GetSize();
            //const size_t AddressSize = arrayType->GetPointerType()->GetSize();

            hr = WriteInt( sourceBuf, sizeof sourceBuf, arrayType->GetLengthType(), value.Array.Length );
            if ( FAILED( hr ) )
                return hr;

            hr = WriteInt( 
                sourceBuf + LengthSize, 
                sizeof sourceBuf - LengthSize, 
                arrayType->GetPointerType(), 
                value.Array.Addr );
        }
        else if ( type->IsAArray() )
        {
            hr = WriteInt( sourceBuf, sizeof sourceBuf, type, value.Addr );
        }
        else if ( type->IsDelegate() )
        {
            MagoEE::Type* voidPtrType = mTypeEnv->GetVoidPointerType();
            const size_t AddressSize = voidPtrType->GetSize();

            hr = WriteInt( sourceBuf, sizeof sourceBuf, voidPtrType, value.Delegate.ContextAddr );
            if ( FAILED( hr ) )
                return hr;

            hr = WriteInt( 
                sourceBuf + AddressSize, 
                sizeof sourceBuf - AddressSize, 
                voidPtrType, 
                value.Delegate.FuncAddr );
        }
        else
            return E_FAIL;

        if ( FAILED( hr ) )
            return hr;

        hr = debuggerProxy->WriteMemory( 
            mThread->GetCoreProcess(), 
            (Address) addr, 
            sourceSize, 
            lenWritten, 
            sourceBuf );
        if ( FAILED( hr ) )
            return hr;
        if ( lenWritten < sourceSize )
            return HRESULT_FROM_WIN32( ERROR_PARTIAL_COPY );

        return S_OK;
    }

    HRESULT ExprContext::ReadMemory( 
        MagoEE::Address addr, 
        uint32_t sizeToRead, 
        uint32_t& sizeRead, 
        uint8_t* buffer )
    {
        HRESULT         hr = S_OK;
        SIZE_T          len = sizeToRead;
        SIZE_T          lenRead = 0;
        SIZE_T          lenUnreadable = 0;
        DebuggerProxy*  debuggerProxy = mThread->GetDebuggerProxy();

        hr = debuggerProxy->ReadMemory(
            mThread->GetCoreProcess(),
            (Address) addr,
            len,
            lenRead,
            lenUnreadable,
            buffer );
        if ( FAILED( hr ) )
            return hr;

        sizeRead = lenRead;

        return S_OK;
    }


    ////////////////////////////////////////////////////////////////////////////// 

    HRESULT ExprContext::Init( 
        Module* module, 
        Thread* thread,
        MagoST::SymHandle funcSH, 
        MagoST::SymHandle blockSH,
        Address pc,
        IRegisterSet* regSet )
    {
        _ASSERT( regSet != NULL );
        _ASSERT( module != NULL );
        _ASSERT( thread != NULL );

        mModule = module;
        mThread = thread;
        mFuncSH = funcSH;
        mBlockSH = blockSH;
        mPC = pc;
        mRegSet = regSet;

        // we have to be able to evaluate expressions even if there aren't symbols

        HRESULT hr = S_OK;

        hr = MagoEE::EED::MakeTypeEnv( mTypeEnv.Ref() );
        if ( FAILED( hr ) )
            return hr;

        hr = MagoEE::EED::MakeNameTable( mStrTable.Ref() );
        if ( FAILED( hr ) )
            return hr;

        return S_OK;
    }


    ////////////////////////////////////////////////////////////////////////////// 

    HRESULT ExprContext::FindLocalSymbol( const char* name, size_t nameLen, MagoST::SymHandle& localSH )
    {
        RefPtr<MagoST::ISession>    session;

        if ( !mModule->GetSymbolSession( session ) )
            return E_NOT_FOUND;

        return FindLocalSymbol( session, mBlockSH, name, nameLen, localSH );
    }

    HRESULT ExprContext::FindGlobalSymbol( const char* name, size_t nameLen, MagoST::SymHandle& globalSH )
    {
        RefPtr<MagoST::ISession>    session;

        if ( !mModule->GetSymbolSession( session ) )
            return E_NOT_FOUND;

        return FindGlobalSymbol( session, name, nameLen, globalSH );
    }

    HRESULT ExprContext::FindLocalSymbol( 
        MagoST::ISession* session, 
        MagoST::SymHandle blockSH,
        const char* name, 
        size_t nameLen, 
        MagoST::SymHandle& localSH )
    {
        HRESULT hr = S_OK;

        // TODO: go down the lexical hierarchy
        hr = session->FindChildSymbol( blockSH, name, (BYTE) nameLen, localSH );
        if ( hr != S_OK )
            return E_NOT_FOUND;

        return S_OK;
    }

    HRESULT ExprContext::FindGlobalSymbol( 
        MagoST::ISession* session, 
        const char* name, 
        size_t nameLen, 
        MagoST::SymHandle& globalSH )
    {
        HRESULT hr = S_OK;
        MagoST::EnumNamedSymbolsData    enumData = { 0 };

        for ( int i = 0; i < MagoST::SymHeap_Count; i++ )
        {
            hr = session->FindFirstSymbol( (MagoST::SymbolHeapId) i, name, (BYTE) nameLen, enumData );
            if ( hr == S_OK )
                break;
        }

        if ( hr != S_OK )
            return E_NOT_FOUND;

        // TODO: for now we only care about the first one
        hr = session->GetCurrentSymbol( enumData, globalSH );
        if ( FAILED( hr ) )
            return hr;

        return S_OK;
    }

    HRESULT ExprContext::GetSession( MagoST::ISession*& session )
    {
        RefPtr<MagoST::ISession>    spSession;

        if ( !mModule->GetSymbolSession( spSession ) )
            return E_FAIL;

        session = spSession.Detach();
        return S_OK;
    }

    MagoEE::ITypeEnv* ExprContext::GetTypeEnv()
    {
        return mTypeEnv;
    }

    MagoEE::NameTable* ExprContext::GetStringTable()
    {
        return mStrTable;
    }

    MagoST::SymHandle ExprContext::GetFunctionSH()
    {
        return mFuncSH;
    }

    MagoST::SymHandle ExprContext::GetBlockSH()
    {
        return mBlockSH;
    }

    // TODO: these declarations can be cached. We can keep a small cache for each module
    HRESULT ExprContext::MakeDeclarationFromSymbol( 
        MagoST::SymHandle handle, 
        MagoEE::Declaration*& decl )
    {
        HRESULT                     hr = S_OK;
        MagoST::SymTag              tag = MagoST::SymTagNull;
        MagoST::SymInfoData         infoData = { 0 };
        MagoST::ISymbolInfo*        symInfo = NULL;
        RefPtr<MagoST::ISession>    session;

        if ( !mModule->GetSymbolSession( session ) )
            return E_NOT_FOUND;

        hr = session->GetSymbolInfo( handle, infoData, symInfo );
        if ( FAILED( hr ) )
            return hr;

        tag = symInfo->GetSymTag();

        switch ( tag )
        {
        case SymTagData:
        case SymTagFunction:
            hr = MakeDeclarationFromDataSymbol( infoData, symInfo, decl );
            break;

        case SymTagTypedef:
            hr = MakeDeclarationFromTypedefSymbol( infoData, symInfo, decl );
            break;

        default:
            return E_FAIL;
        }

        return hr;
    }

    HRESULT ExprContext::MakeDeclarationFromSymbol( 
        MagoST::TypeHandle handle, 
        MagoEE::Declaration*& decl )
    {
        HRESULT hr = S_OK;
        MagoST::SymTag              tag = MagoST::SymTagNull;
        MagoST::SymInfoData         infoData = { 0 };
        MagoST::ISymbolInfo*        symInfo = NULL;
        RefPtr<MagoST::ISession>    session;

        if ( !mModule->GetSymbolSession( session ) )
            return E_NOT_FOUND;

        hr = session->GetTypeInfo( handle, infoData, symInfo );
        if ( FAILED( hr ) )
            return hr;

        tag = symInfo->GetSymTag();

        switch ( tag )
        {
        case SymTagData:
            hr = MakeDeclarationFromDataSymbol( infoData, symInfo, decl );
            break;

        default:
            return E_FAIL;
        }

        return hr;
    }

    HRESULT ExprContext::MakeDeclarationFromDataSymbol( 
        const MagoST::SymInfoData& infoData,
        MagoST::ISymbolInfo* symInfo, 
        MagoEE::Declaration*& decl )
    {
        HRESULT                 hr = S_OK;
        MagoST::TypeIndex       typeIndex = 0;
        RefPtr<MagoEE::Type>    type;

        if ( !symInfo->GetType( typeIndex ) )
            return E_FAIL;

        hr = GetTypeFromTypeSymbol( typeIndex, type.Ref() );
        if ( FAILED( hr ) )
            return hr;

        return MakeDeclarationFromDataSymbol( infoData, symInfo, type, decl );
    }

    HRESULT ExprContext::MakeDeclarationFromDataSymbol( 
        const MagoST::SymInfoData& infoData,
        MagoST::ISymbolInfo* symInfo, 
        MagoEE::Type* type,
        MagoEE::Declaration*& decl )
    {
        MagoST::LocationType    loc;
        RefPtr<GeneralCVDecl>   cvDecl;

        if ( !symInfo->GetLocation( loc ) )
            return E_FAIL;

        switch ( loc )
        {
        case LocIsRegRel:
        case LocIsBitField:
        case LocIsConstant:
        case LocIsEnregistered:
        case LocIsStatic:
        case LocIsThisRel:
        case LocIsTLS:
            cvDecl = new GeneralCVDecl( this, infoData, symInfo );
            break;

        default:
            return E_FAIL;
        }

        if ( cvDecl == NULL )
            return E_OUTOFMEMORY;

        cvDecl->SetType( type );

        decl = cvDecl.Detach();
        return S_OK;
    }

    HRESULT ExprContext::MakeDeclarationFromTypedefSymbol( 
        const MagoST::SymInfoData& infoData, 
        MagoST::ISymbolInfo* symInfo, 
        MagoEE::Declaration*& decl )
    {
        HRESULT                 hr = S_OK;
        MagoST::TypeIndex       typeIndex = 0;
        PasString*              pstrName1 = NULL;
        MagoST::TypeHandle      handle = { 0 };
        MagoST::SymInfoData     typeInfoData = { 0 };
        MagoST::ISymbolInfo*    typeInfo = NULL;
        PasString*              pstrName2 = NULL;
        RefPtr<MagoEE::Type>    refType;
        RefPtr<MagoST::ISession>    session;

        if ( !mModule->GetSymbolSession( session ) )
            return E_NOT_FOUND;

        if ( !symInfo->GetType( typeIndex ) )
            return E_FAIL;

        if ( !symInfo->GetName( pstrName1 ) )
            return E_FAIL;

        if ( !session->GetTypeFromTypeIndex( typeIndex, handle ) )
            return E_FAIL;

        hr = session->GetTypeInfo( handle, typeInfoData, typeInfo );
        if ( FAILED( hr ) )
            return hr;

        // it doesn't have to have a name: for example a pointer type
        typeInfo->GetName( pstrName2 );

        hr = GetTypeFromTypeSymbol( typeIndex, refType.Ref() );
        if ( FAILED( hr ) )
            return hr;

        if ( (pstrName2 != NULL)
            && (pstrName1->len == pstrName2->len) 
            && (strncmp( pstrName1->name, pstrName2->name, pstrName1->len ) == 0) )
        {
            // the typedef has the same name as the type, 
            // so let's use the referenced type directly, as if there's no typedef

            decl = refType->GetDeclaration().Detach();
        }
        else
        {
            RefPtr<GeneralCVDecl>   cvDecl;
            RefPtr<MagoEE::Type>    type;

            cvDecl = new GeneralCVDecl( this, infoData, symInfo );
            if ( cvDecl == NULL )
                return E_OUTOFMEMORY;

            hr = mTypeEnv->NewTypedef( cvDecl->GetName(), refType, type.Ref() );
            if ( FAILED( hr ) )
                return hr;

            cvDecl->SetType( type );

            decl = cvDecl.Detach();
        }

        return S_OK;
    }

    MagoEE::ENUMTY ExprContext::GetBasicTy( DWORD diaBaseTypeId, DWORD size )
    {
        MagoEE::ENUMTY  ty = MagoEE::Tnone;

        // TODO: what about ifloat, idouble, ireal?
        switch ( diaBaseTypeId )
        {
        case btVoid:
            ty = MagoEE::Tvoid;
            break;

        case btChar:
            switch ( size )
            {
            case 1: ty = MagoEE::Tchar; break;
            case 4: ty = MagoEE::Tdchar; break;
            }
            break;

        case btWChar:
            ty = MagoEE::Twchar;
            break;

        case btInt:
        case btLong:
            switch ( size )
            {
            case 1: ty = MagoEE::Tint8; break;
            case 2: ty = MagoEE::Tint16; break;
            case 4: ty = MagoEE::Tint32; break;
            case 8: ty = MagoEE::Tint64; break;
            }
            break;

        case btUInt:
        case btULong:
            switch ( size )
            {
            case 1: ty = MagoEE::Tuns8; break;
            case 2: ty = MagoEE::Tuns16; break;
            case 4: ty = MagoEE::Tuns32; break;
            case 8: ty = MagoEE::Tuns64; break;
            }
            break;

        case btFloat:
            switch ( size )
            {
            case 4: ty = MagoEE::Tfloat32;  break;
            case 8: ty = MagoEE::Tfloat64;  break;
            case 10: ty = MagoEE::Tfloat80;  break;
            }
            break;

        case btBool:
            ty = MagoEE::Tbool;
            break;

        case btComplex:
            switch ( size )
            {
            case 8: ty = MagoEE::Tcomplex32;  break;
            case 16: ty = MagoEE::Tcomplex64;  break;
            case 20: ty = MagoEE::Tcomplex80;  break;
            }
            break;
        }

        return ty;
    }

    HRESULT ExprContext::GetTypeFromTypeSymbol( 
        MagoST::TypeIndex typeIndex,
        MagoEE::Type*& type )
    {
        HRESULT                 hr = S_OK;
        MagoST::SymTag          tag = MagoST::SymTagNull;
        MagoST::TypeHandle      typeTH = { 0 };
        MagoST::SymInfoData     infoData = { 0 };
        MagoST::ISymbolInfo*    symInfo = NULL;
        RefPtr<MagoST::ISession>    session;

        if ( !mModule->GetSymbolSession( session ) )
            return E_FAIL;

        if ( !session->GetTypeFromTypeIndex( typeIndex, typeTH ) )
            return E_FAIL;

        hr = session->GetTypeInfo( typeTH, infoData, symInfo );
        if ( FAILED( hr ) )
            return hr;

        tag = symInfo->GetSymTag();

        switch ( tag )
        {
        case SymTagUDT:
        case SymTagEnum:
            return GetUdtTypeFromTypeSymbol( typeTH, infoData, symInfo, type );

        case SymTagFunctionType:
            return GetFunctionTypeFromTypeSymbol( typeTH, infoData, symInfo, type );

        case SymTagBaseType:
            return GetBasicTypeFromTypeSymbol( infoData, symInfo, type );

        case SymTagPointerType:
            {
                RefPtr<MagoEE::Type>    pointed;
                RefPtr<MagoEE::Type>    pointer;
                MagoST::TypeIndex       pointedTypeIndex = 0;

                if ( !symInfo->GetType( pointedTypeIndex ) )
                    return E_NOT_FOUND;

                hr = GetTypeFromTypeSymbol( pointedTypeIndex, pointed.Ref() );
                if ( FAILED( hr ) )
                    return E_NOT_FOUND;

                hr = mTypeEnv->NewPointer( pointed, pointer.Ref() );
                if ( hr != S_OK )
                    return E_NOT_FOUND;

                type = pointer.Detach();
                return S_OK;
            }
            break;

        case SymTagArrayType:
            {
                RefPtr<MagoEE::Type>    elemType;
                RefPtr<MagoEE::Type>    arrayType;
                uint32_t                count = 0;
                MagoST::TypeIndex       elemTypeIndex = 0;

                if ( !symInfo->GetType( elemTypeIndex ) )
                    return E_NOT_FOUND;

                if ( !symInfo->GetCount( count ) )
                    return E_NOT_FOUND;

                hr = GetTypeFromTypeSymbol( elemTypeIndex, elemType.Ref() );
                if ( FAILED( hr ) )
                    return hr;

                hr = mTypeEnv->NewSArray( elemType, count, arrayType.Ref() );
                if ( FAILED( hr ) )
                    return hr;

                type = arrayType.Detach();
                return S_OK;
            }
            break;

        case SymTagTypedef:
            _ASSERT( false );
            break;

        case SymTagCustomType:
            return GetCustomTypeFromTypeSymbol( infoData, symInfo, type );

        //case SymTagManagedType:
        default:
            return E_FAIL;
        }

        return E_FAIL;
    }

    HRESULT ExprContext::GetFunctionTypeFromTypeSymbol( 
        MagoST::TypeHandle typeHandle,
        const MagoST::SymInfoData& infoData,
        MagoST::ISymbolInfo* symInfo,
        MagoEE::Type*& type )
    {
        HRESULT hr = S_OK;
        RefPtr<MagoEE::ParameterList>   params;
        MagoST::TypeIndex       retTI = 0;
        RefPtr<MagoEE::Type>    retType;
        uint32_t                paramCount = 0;
        MagoST::TypeIndex       paramListTI = 0;
        MagoST::TypeHandle      paramListTH = { 0 };
        MagoST::SymInfoData     paramListInfoData = { 0 };
        MagoST::ISymbolInfo*    paramListInfo = NULL;
        MagoST::TypeIndex*      paramTIs = NULL;
        RefPtr<MagoST::ISession>    session;

        if ( !mModule->GetSymbolSession( session ) )
            return E_NOT_FOUND;

        if ( !symInfo->GetType( retTI ) )
            return E_FAIL;

        hr = GetTypeFromTypeSymbol( retTI, retType.Ref() );
        if ( FAILED( hr ) )
            return hr;

        hr = mTypeEnv->NewParams( params.Ref() );
        if ( FAILED( hr ) )
            return hr;

        if ( !symInfo->GetParamList( paramListTI ) )
            return E_FAIL;

        if ( !session->GetTypeFromTypeIndex( paramListTI, paramListTH ) )
            return E_FAIL;

        hr = session->GetTypeInfo( paramListTH, paramListInfoData, paramListInfo );
        if ( FAILED( hr ) )
            return hr;

        if ( !paramListInfo->GetCount( paramCount ) )
            return E_FAIL;

        if ( !paramListInfo->GetTypes( paramTIs ) )
            return E_FAIL;

        for ( uint32_t i = 0; i < paramCount; i++ )
        {
            RefPtr<MagoEE::Type>        paramType;
            RefPtr<MagoEE::Parameter>   param;

            hr = GetTypeFromTypeSymbol( paramTIs[i], paramType.Ref() );
            if ( FAILED( hr ) )
                return hr;

            hr = mTypeEnv->NewParam( 0, paramType, param.Ref() );
            if ( FAILED( hr ) )
                return hr;

            params->List.push_back( param );
        }

        // TODO: calling convention/var args
        hr = mTypeEnv->NewFunction( retType, params, 0, type );
        if ( FAILED( hr ) )
            return hr;

        return S_OK;
    }

    HRESULT ExprContext::GetUdtTypeFromTypeSymbol( 
        MagoST::TypeHandle typeHandle,
        const MagoST::SymInfoData& infoData,
        MagoST::ISymbolInfo* symInfo,
        MagoEE::Type*& type )
    {
        RefPtr<MagoEE::Declaration> decl;

        decl = new TypeCVDecl( this, typeHandle, infoData, symInfo );
        if ( decl == NULL )
            return E_OUTOFMEMORY;

        if ( !decl->GetType( type ) )
            return E_NOT_FOUND;

        return S_OK;
    }

    HRESULT ExprContext::GetBasicTypeFromTypeSymbol( 
            const MagoST::SymInfoData& infoData,
            MagoST::ISymbolInfo* symInfo,
            MagoEE::Type*& type )
    {
        DWORD           baseTypeId = 0;
        uint32_t        size = 0;
        MagoEE::ENUMTY  ty = MagoEE::Tnone;

        if ( !symInfo->GetBasicType( baseTypeId ) )
            return E_NOT_FOUND;

        if ( !symInfo->GetLength( size ) )
            return E_NOT_FOUND;

        ty = GetBasicTy( baseTypeId, size );
        if ( ty == MagoEE::Tnone )
            return E_NOT_FOUND;

        type = mTypeEnv->GetType( ty );
        if ( type == NULL )
            return E_NOT_FOUND;

        type->AddRef();
        return S_OK;
    }

    HRESULT ExprContext::GetCustomTypeFromTypeSymbol( 
        const MagoST::SymInfoData& infoData, 
        MagoST::ISymbolInfo* symInfo,
        MagoEE::Type*& type )
    {
        HRESULT     hr = S_OK;
        uint32_t    count = 0;
        uint32_t    oemId = 0;
        uint32_t    oemSymId = 0;

        if ( !symInfo->GetOemId( oemId ) )
            return E_FAIL;
        if ( oemId != DMD_OEM )
            return E_FAIL;

        if ( !symInfo->GetOemSymbolId( oemSymId ) )
            return E_FAIL;

        if ( !symInfo->GetCount( count ) )
            return E_FAIL;

        switch ( oemSymId )
        {
        case DMD_OEM_DARRAY:
        case DMD_OEM_AARRAY:
        case DMD_OEM_DELEGATE:
            {
                RefPtr<MagoEE::Type>    types[2];
                MagoST::TypeIndex*      typeIndexes = NULL;

                if ( count != 2 )
                    return E_FAIL;

                if ( !symInfo->GetTypes( typeIndexes ) )
                    return E_FAIL;

                for ( uint32_t i = 0; i < count; i++ )
                {
                    hr = GetTypeFromTypeSymbol( typeIndexes[i], types[i].Ref() );
                    if ( FAILED( hr ) )
                        return hr;
                }

                switch ( oemSymId )
                {
                case DMD_OEM_DARRAY:
                    hr = mTypeEnv->NewDArray( types[1], type );
                    break;

                case DMD_OEM_AARRAY:
                    hr = mTypeEnv->NewAArray( types[1], types[0], type );
                    break;

                case DMD_OEM_DELEGATE:
                    hr = mTypeEnv->NewDelegate( types[1], type );
                    break;
                }

                return hr;
            }
            break;

        default:
            return E_FAIL;
        }
    }


    static const uint8_t gRegMapX86[] = 
    {
        RegX86_NONE,
        RegX86_AL,
        RegX86_CL,
        RegX86_DL,
        RegX86_BL,
        RegX86_AH,
        RegX86_CH,
        RegX86_DH,
        RegX86_BH,
        RegX86_AX,
        RegX86_CX,
        RegX86_DX,
        RegX86_BX,
        RegX86_SP,
        RegX86_BP,
        RegX86_SI,
        RegX86_DI,
        RegX86_EAX,
        RegX86_ECX,
        RegX86_EDX,
        RegX86_EBX,
        RegX86_ESP,
        RegX86_EBP,
        RegX86_ESI,
        RegX86_EDI,
        RegX86_ES,
        RegX86_CS,
        RegX86_SS,
        RegX86_DS,
        RegX86_FS,
        RegX86_GS,
        RegX86_IP,
        RegX86_FLAGS,
        RegX86_EIP,
        RegX86_EFLAGS,

        0,
        0,
        0,
        0,
        0,

        0, //RegX86_TEMP,
        0, //RegX86_TEMPH,
        0, //RegX86_QUOTE,
        0, //RegX86_PCDR3,
        0, //RegX86_PCDR4,
        0, //RegX86_PCDR5,
        0, //RegX86_PCDR6,
        0, //RegX86_PCDR7,

        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,

        0, //RegX86_CR0,
        0, //RegX86_CR1,
        0, //RegX86_CR2,
        0, //RegX86_CR3,
        0, //RegX86_CR4,

        0,
        0,
        0,
        0,
        0,

        RegX86_DR0,
        RegX86_DR1,
        RegX86_DR2,
        RegX86_DR3,
        0, //RegX86_DR4,
        0, //RegX86_DR5,
        RegX86_DR6,
        RegX86_DR7,

        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,

        0, //RegX86_GDTR,
        0, //RegX86_GDTL,
        0, //RegX86_IDTR,
        0, //RegX86_IDTL,
        0, //RegX86_LDTR,
        0, //RegX86_TR,

        0, //RegX86_PSEUDO1,
        0, //RegX86_PSEUDO2,
        0, //RegX86_PSEUDO3,
        0, //RegX86_PSEUDO4,
        0, //RegX86_PSEUDO5,
        0, //RegX86_PSEUDO6,
        0, //RegX86_PSEUDO7,
        0, //RegX86_PSEUDO8,
        0, //RegX86_PSEUDO9,

        0,
        0,
        0,

        RegX86_ST0,
        RegX86_ST1,
        RegX86_ST2,
        RegX86_ST3,
        RegX86_ST4,
        RegX86_ST5,
        RegX86_ST6,
        RegX86_ST7,
        RegX86_CTRL,
        RegX86_STAT,
        RegX86_TAG,
        RegX86_FPIP,
        RegX86_FPCS,
        RegX86_FPDO,
        RegX86_FPDS,
        0,
        RegX86_FPEIP,
        RegX86_FPEDO,

        RegX86_MM0,
        RegX86_MM1,
        RegX86_MM2,
        RegX86_MM3,
        RegX86_MM4,
        RegX86_MM5,
        RegX86_MM6,
        RegX86_MM7,

        RegX86_XMM0,
        RegX86_XMM1,
        RegX86_XMM2,
        RegX86_XMM3,
        RegX86_XMM4,
        RegX86_XMM5,
        RegX86_XMM6,
        RegX86_XMM7,

        RegX86_XMM00,
        RegX86_XMM01,
        RegX86_XMM02,
        RegX86_XMM03,
        RegX86_XMM10,
        RegX86_XMM11,
        RegX86_XMM12,
        RegX86_XMM13,
        RegX86_XMM20,
        RegX86_XMM21,
        RegX86_XMM22,
        RegX86_XMM23,
        RegX86_XMM30,
        RegX86_XMM31,
        RegX86_XMM32,
        RegX86_XMM33,
        RegX86_XMM40,
        RegX86_XMM41,
        RegX86_XMM42,
        RegX86_XMM43,
        RegX86_XMM50,
        RegX86_XMM51,
        RegX86_XMM52,
        RegX86_XMM53,
        RegX86_XMM60,
        RegX86_XMM61,
        RegX86_XMM62,
        RegX86_XMM63,
        RegX86_XMM70,
        RegX86_XMM71,
        RegX86_XMM72,
        RegX86_XMM73,

        RegX86_XMM0L,
        RegX86_XMM1L,
        RegX86_XMM2L,
        RegX86_XMM3L,
        RegX86_XMM4L,
        RegX86_XMM5L,
        RegX86_XMM6L,
        RegX86_XMM7L,

        RegX86_XMM0H,
        RegX86_XMM1H,
        RegX86_XMM2H,
        RegX86_XMM3H,
        RegX86_XMM4H,
        RegX86_XMM5H,
        RegX86_XMM6H,
        RegX86_XMM7H,

        0,

        RegX86_MXCSR,

        0, //EDXEAX
        0,
        0,
        0,
        0,
        0,
        0,
        0,

        RegX86_EMM0L,
        RegX86_EMM1L,
        RegX86_EMM2L,
        RegX86_EMM3L,
        RegX86_EMM4L,
        RegX86_EMM5L,
        RegX86_EMM6L,
        RegX86_EMM7L,

        RegX86_EMM0H,
        RegX86_EMM1H,
        RegX86_EMM2H,
        RegX86_EMM3H,
        RegX86_EMM4H,
        RegX86_EMM5H,
        RegX86_EMM6H,
        RegX86_EMM7H,

        RegX86_MM00,
        RegX86_MM01,
        RegX86_MM10,
        RegX86_MM11,
        RegX86_MM20,
        RegX86_MM21,
        RegX86_MM30,
        RegX86_MM31,
        RegX86_MM40,
        RegX86_MM41,
        RegX86_MM50,
        RegX86_MM51,
        RegX86_MM60,
        RegX86_MM61,
        RegX86_MM70,
        RegX86_MM71,
    };

    C_ASSERT( _countof( gRegMapX86 ) == 252 );


    HRESULT ExprContext::GetRegValue( DWORD reg, MagoEE::DataValueKind& kind, MagoEE::DataValue& value )
    {
        HRESULT         hr = S_OK;
        RegisterValue   regVal = { 0 };

        if ( reg == CV_REG_EDXEAX )
        {
            hr = mRegSet->GetValue( RegX86_EDX, regVal );
            if ( FAILED( hr ) )
                return hr;

            value.UInt64Value = regVal.Value.I32;
            value.UInt64Value <<= 32;

            hr = mRegSet->GetValue( RegX86_EAX, regVal );
            if ( FAILED( hr ) )
                return hr;

            value.UInt64Value |= regVal.Value.I32;
            kind = MagoEE::DataValueKind_UInt64;
        }
        else
        {
            if ( reg >= _countof( gRegMapX86 ) )
                return E_FAIL;

            hr = mRegSet->GetValue( gRegMapX86[reg], regVal );
            if ( FAILED( hr ) )
                return hr;

            switch ( regVal.Type )
            {
            case RegType_Int8:  value.UInt64Value = regVal.Value.I8;    kind = MagoEE::DataValueKind_UInt64;    break;
            case RegType_Int16: value.UInt64Value = regVal.Value.I16;   kind = MagoEE::DataValueKind_UInt64;    break;
            case RegType_Int32: value.UInt64Value = regVal.Value.I32;   kind = MagoEE::DataValueKind_UInt64;    break;
            case RegType_Int64: value.UInt64Value = regVal.Value.I64;   kind = MagoEE::DataValueKind_UInt64;    break;

            case RegType_Float32:
                value.Float80Value.FromFloat( regVal.Value.F32 );
                kind = MagoEE::DataValueKind_Float80;
                break;

            case RegType_Float64:
                value.Float80Value.FromDouble( regVal.Value.F64 );
                kind = MagoEE::DataValueKind_Float80;
                break;

            case RegType_Float80:   
                memcpy( &value.Float80Value, regVal.Value.F80Bytes, sizeof value.Float80Value );
                kind = MagoEE::DataValueKind_Float80;
                break;

            default:
                return E_FAIL;
            }
        }

        return S_OK;
    }
}