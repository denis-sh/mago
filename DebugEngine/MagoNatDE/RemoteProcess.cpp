/*
   Copyright (c) 2013 Aldo J. Nunez

   Licensed under the Apache License, Version 2.0.
   See the LICENSE text file for details.
*/

#pragma once

#include "Common.h"
#include "RemoteProcess.h"
#include "RemoteDebuggerProxy.h"
#include "ArchData.h"


namespace Mago
{
    //------------------------------------------------------------------------
    //  RemoteProcess
    //------------------------------------------------------------------------

    RemoteProcess::RemoteProcess()
        :   mRefCount( 0 ),
            mPid( 0 ),
            mCreateMethod( Create_Launch ),
            mMachineType( 0 )
    {
    }

    void RemoteProcess::AddRef()
    {
        InterlockedIncrement( &mRefCount );
    }

    void RemoteProcess::Release()
    {
        long ref = InterlockedDecrement( &mRefCount );
        _ASSERT( ref >= 0 );
        if ( ref == 0 )
        {
            delete this;
        }
    }

    CreateMethod RemoteProcess::GetCreateMethod()
    {
        return mCreateMethod;
    }

    uint32_t RemoteProcess::GetPid()
    {
        return mPid;
    }

    const wchar_t* RemoteProcess::GetExePath()
    {
        return mExePath.c_str();
    }

    uint16_t RemoteProcess::GetMachineType()
    {
        return mMachineType;
    }

    ArchData* RemoteProcess::GetArchData()
    {
        return mArchData.Get();
    }

    CoreProcessType RemoteProcess::GetProcessType()
    {
        return CoreProcess_Remote;
    }

    void RemoteProcess::Init(
            uint32_t pid, 
            const wchar_t* exePath, 
            CreateMethod createMethod, 
            uint16_t machineType,
            ArchData* archData )
    {
        _ASSERT( pid != 0 );
        _ASSERT( exePath != NULL );
        _ASSERT( machineType != 0 );
        _ASSERT( archData != NULL );

        mPid = pid;
        mExePath = exePath;
        mCreateMethod = createMethod;
        mMachineType = machineType;
        mArchData = archData;
    }


    //------------------------------------------------------------------------
    //  RemoteThread
    //------------------------------------------------------------------------

    RemoteThread::RemoteThread( uint32_t tid, Address64 startAddr, Address64 tebBase )
        :   mRefCount( 0 ),
            mTid( tid ),
            mStartAddr( startAddr ),
            mTebBase( tebBase )
    {
        _ASSERT( tid != 0 );
    }

    void RemoteThread::AddRef()
    {
        InterlockedIncrement( &mRefCount );
    }

    void RemoteThread::Release()
    {
        long ref = InterlockedDecrement( &mRefCount );
        _ASSERT( ref >= 0 );
        if ( ref == 0 )
        {
            delete this;
        }
    }

    uint32_t RemoteThread::GetTid()
    {
        return mTid;
    }

    Address64 RemoteThread::GetStartAddr()
    {
        return mStartAddr;
    }

    Address64 RemoteThread::GetTebBase()
    {
        return mTebBase;
    }

    CoreProcessType RemoteThread::GetProcessType()
    {
        return CoreProcess_Remote;
    }


    //------------------------------------------------------------------------
    //  RemoteModule
    //------------------------------------------------------------------------

    RemoteModule::RemoteModule( 
            RemoteDebuggerProxy* debuggerProxy,
            Address64 imageBase, 
            Address64 prefImageBase, 
            uint32_t size, 
            uint16_t machineType, 
            const wchar_t* path )
        :   mRefCount( 0 ),
            mDebuggerProxy( debuggerProxy ),
            mImageBase( imageBase ),
            mPrefImageBase( prefImageBase ),
            mSize( size ),
            mMachineType( machineType ),
            mPath( path )
    {
        _ASSERT( imageBase != 0 );
        _ASSERT( size != 0 );
        _ASSERT( path != NULL );
    }

    void RemoteModule::AddRef()
    {
        InterlockedIncrement( &mRefCount );
    }

    void RemoteModule::Release()
    {
        long ref = InterlockedDecrement( &mRefCount );
        _ASSERT( ref >= 0 );
        if ( ref == 0 )
        {
            delete this;
        }
    }

    Address64 RemoteModule::GetImageBase()
    {
        return mImageBase;
    }

    Address64 RemoteModule::GetPreferredImageBase()
    {
        return mPrefImageBase;
    }

    uint32_t RemoteModule::GetSize()
    {
        return mSize;
    }

    uint16_t RemoteModule::GetMachine()
    {
        return mMachineType;
    }

    const wchar_t* RemoteModule::GetPath()
    {
        return mPath.c_str();
    }

    const wchar_t* RemoteModule::GetSymbolSearchPath()
    {
        return mDebuggerProxy->GetSymbolSearchPath().data ();
    }
}
