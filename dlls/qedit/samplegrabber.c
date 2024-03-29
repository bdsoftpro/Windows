/*              DirectShow Sample Grabber object (QEDIT.DLL)
 *
 * Copyright 2009 Paul Chitescu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <assert.h>
#include <stdarg.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "ole2.h"

#include "qedit_private.h"
#include "wine/debug.h"
#include "wine/strmbase.h"

WINE_DEFAULT_DEBUG_CHANNEL(qedit);

/* Sample Grabber filter implementation */
typedef struct _SG_Impl {
    struct strmbase_filter filter;
    ISampleGrabber ISampleGrabber_iface;

    struct strmbase_source source;
    /* IMediaSeeking and IMediaPosition are implemented by ISeekingPassThru */
    IUnknown *seekthru_unk;
    IMemInputPin *memOutput;

    struct strmbase_sink sink;
    AM_MEDIA_TYPE mtype;
    IMemInputPin IMemInputPin_iface;
    IMemAllocator *allocator;

    ISampleGrabberCB *grabberIface;
    LONG grabberMethod;
    LONG oneShot;
    LONG bufferLen;
    void* bufferData;
} SG_Impl;

enum {
    OneShot_None,
    OneShot_Wait,
    OneShot_Past,
};

static inline SG_Impl *impl_from_strmbase_filter(struct strmbase_filter *iface)
{
    return CONTAINING_RECORD(iface, SG_Impl, filter);
}

static inline SG_Impl *impl_from_ISampleGrabber(ISampleGrabber *iface)
{
    return CONTAINING_RECORD(iface, SG_Impl, ISampleGrabber_iface);
}

static inline SG_Impl *impl_from_IMemInputPin(IMemInputPin *iface)
{
    return CONTAINING_RECORD(iface, SG_Impl, IMemInputPin_iface);
}


/* Cleanup at end of life */
static void SampleGrabber_cleanup(SG_Impl *This)
{
    TRACE("(%p)\n", This);
    if (This->filter.filterInfo.pGraph)
        WARN("(%p) still joined to filter graph %p\n", This, This->filter.filterInfo.pGraph);
    if (This->allocator)
        IMemAllocator_Release(This->allocator);
    if (This->memOutput)
        IMemInputPin_Release(This->memOutput);
    if (This->grabberIface)
        ISampleGrabberCB_Release(This->grabberIface);
    CoTaskMemFree(This->mtype.pbFormat);
    CoTaskMemFree(This->bufferData);
    if(This->seekthru_unk)
        IUnknown_Release(This->seekthru_unk);
}

static struct strmbase_pin *sample_grabber_get_pin(struct strmbase_filter *iface, unsigned int index)
{
    SG_Impl *filter = impl_from_strmbase_filter(iface);

    if (index == 0)
        return &filter->sink.pin;
    else if (index == 1)
        return &filter->source.pin;
    return NULL;
}

static void sample_grabber_destroy(struct strmbase_filter *iface)
{
    SG_Impl *filter = impl_from_strmbase_filter(iface);

    SampleGrabber_cleanup(filter);
    strmbase_filter_cleanup(&filter->filter);
    CoTaskMemFree(filter);
}

static HRESULT sample_grabber_query_interface(struct strmbase_filter *iface, REFIID iid, void **out)
{
    SG_Impl *filter = impl_from_strmbase_filter(iface);

    if (IsEqualGUID(iid, &IID_ISampleGrabber))
        *out = &filter->ISampleGrabber_iface;
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static const struct strmbase_filter_ops filter_ops =
{
    .filter_get_pin = sample_grabber_get_pin,
    .filter_destroy = sample_grabber_destroy,
    .filter_query_interface = sample_grabber_query_interface,
};

/* Helper that buffers data and/or calls installed sample callbacks */
static void SampleGrabber_callback(SG_Impl *This, IMediaSample *sample)
{
    double time = 0.0;
    REFERENCE_TIME tStart, tEnd;
    if (This->bufferLen >= 0) {
        BYTE *data = 0;
        LONG size = IMediaSample_GetActualDataLength(sample);
        if (size >= 0 && SUCCEEDED(IMediaSample_GetPointer(sample, &data))) {
            if (!data)
                size = 0;
            EnterCriticalSection(&This->filter.csFilter);
            if (This->bufferLen != size) {
                CoTaskMemFree(This->bufferData);
                This->bufferData = size ? CoTaskMemAlloc(size) : NULL;
                This->bufferLen = size;
            }
            if (size)
                CopyMemory(This->bufferData, data, size);
            LeaveCriticalSection(&This->filter.csFilter);
        }
    }
    if (!This->grabberIface)
        return;
    if (SUCCEEDED(IMediaSample_GetTime(sample, &tStart, &tEnd)))
        time = 1e-7 * tStart;
    switch (This->grabberMethod) {
        case 0:
	    {
		ULONG ref = IMediaSample_AddRef(sample);
		ISampleGrabberCB_SampleCB(This->grabberIface, time, sample);
		ref = IMediaSample_Release(sample) + 1 - ref;
		if (ref)
		{
		    ERR("(%p) Callback referenced sample %p by %u\n", This, sample, ref);
		    /* ugly as hell but some apps are sooo buggy */
		    while (ref--)
			IMediaSample_Release(sample);
		}
	    }
            break;
        case 1:
            {
                BYTE *data = 0;
                LONG size = IMediaSample_GetActualDataLength(sample);
                if (size && SUCCEEDED(IMediaSample_GetPointer(sample, &data)) && data)
                    ISampleGrabberCB_BufferCB(This->grabberIface, time, data, size);
            }
            break;
        case -1:
            break;
        default:
            FIXME("unsupported method %d\n", This->grabberMethod);
            /* do not bother us again */
            This->grabberMethod = -1;
    }
}

/* IUnknown */
static HRESULT WINAPI
SampleGrabber_ISampleGrabber_QueryInterface(ISampleGrabber *iface, REFIID riid, void **ppv)
{
    SG_Impl *This = impl_from_ISampleGrabber(iface);
    return IUnknown_QueryInterface(This->filter.outer_unk, riid, ppv);
}

/* IUnknown */
static ULONG WINAPI
SampleGrabber_ISampleGrabber_AddRef(ISampleGrabber *iface)
{
    SG_Impl *This = impl_from_ISampleGrabber(iface);
    return IUnknown_AddRef(This->filter.outer_unk);
}

/* IUnknown */
static ULONG WINAPI
SampleGrabber_ISampleGrabber_Release(ISampleGrabber *iface)
{
    SG_Impl *This = impl_from_ISampleGrabber(iface);
    return IUnknown_Release(This->filter.outer_unk);
}

/* ISampleGrabber */
static HRESULT WINAPI
SampleGrabber_ISampleGrabber_SetOneShot(ISampleGrabber *iface, BOOL oneShot)
{
    SG_Impl *This = impl_from_ISampleGrabber(iface);
    TRACE("(%p)->(%u)\n", This, oneShot);
    This->oneShot = oneShot ? OneShot_Wait : OneShot_None;
    return S_OK;
}

/* ISampleGrabber */
static HRESULT WINAPI
SampleGrabber_ISampleGrabber_SetMediaType(ISampleGrabber *iface, const AM_MEDIA_TYPE *type)
{
    SG_Impl *This = impl_from_ISampleGrabber(iface);
    TRACE("(%p)->(%p)\n", This, type);
    if (!type)
        return E_POINTER;
    TRACE("Media type: %s/%s ssize: %u format: %s (%u bytes)\n",
	debugstr_guid(&type->majortype), debugstr_guid(&type->subtype),
	type->lSampleSize,
	debugstr_guid(&type->formattype), type->cbFormat);
    CoTaskMemFree(This->mtype.pbFormat);
    This->mtype = *type;
    This->mtype.pUnk = NULL;
    if (type->cbFormat) {
        This->mtype.pbFormat = CoTaskMemAlloc(type->cbFormat);
        CopyMemory(This->mtype.pbFormat, type->pbFormat, type->cbFormat);
    }
    else
        This->mtype.pbFormat = NULL;
    return S_OK;
}

/* ISampleGrabber */
static HRESULT WINAPI
SampleGrabber_ISampleGrabber_GetConnectedMediaType(ISampleGrabber *iface, AM_MEDIA_TYPE *type)
{
    SG_Impl *This = impl_from_ISampleGrabber(iface);
    TRACE("(%p)->(%p)\n", This, type);
    if (!type)
        return E_POINTER;
    if (!This->sink.pin.peer)
        return VFW_E_NOT_CONNECTED;
    *type = This->mtype;
    if (type->cbFormat) {
        type->pbFormat = CoTaskMemAlloc(type->cbFormat);
        CopyMemory(type->pbFormat, This->mtype.pbFormat, type->cbFormat);
    }
    return S_OK;
}

/* ISampleGrabber */
static HRESULT WINAPI
SampleGrabber_ISampleGrabber_SetBufferSamples(ISampleGrabber *iface, BOOL bufferEm)
{
    SG_Impl *This = impl_from_ISampleGrabber(iface);
    TRACE("(%p)->(%u)\n", This, bufferEm);
    EnterCriticalSection(&This->filter.csFilter);
    if (bufferEm) {
        if (This->bufferLen < 0)
            This->bufferLen = 0;
    }
    else
        This->bufferLen = -1;
    LeaveCriticalSection(&This->filter.csFilter);
    return S_OK;
}

/* ISampleGrabber */
static HRESULT WINAPI
SampleGrabber_ISampleGrabber_GetCurrentBuffer(ISampleGrabber *iface, LONG *bufSize, LONG *buffer)
{
    SG_Impl *This = impl_from_ISampleGrabber(iface);
    HRESULT ret = S_OK;
    TRACE("(%p)->(%p, %p)\n", This, bufSize, buffer);
    if (!bufSize)
        return E_POINTER;
    EnterCriticalSection(&This->filter.csFilter);
    if (!This->sink.pin.peer)
        ret = VFW_E_NOT_CONNECTED;
    else if (This->bufferLen < 0)
        ret = E_INVALIDARG;
    else if (This->bufferLen == 0)
        ret = VFW_E_WRONG_STATE;
    else {
        if (buffer) {
            if (*bufSize >= This->bufferLen)
                CopyMemory(buffer, This->bufferData, This->bufferLen);
            else
                ret = E_OUTOFMEMORY;
        }
        *bufSize = This->bufferLen;
    }
    LeaveCriticalSection(&This->filter.csFilter);
    return ret;
}

/* ISampleGrabber */
static HRESULT WINAPI
SampleGrabber_ISampleGrabber_GetCurrentSample(ISampleGrabber *iface, IMediaSample **sample)
{
    /* MS doesn't implement it either, no one should call it */
    WARN("(%p): not implemented\n", sample);
    return E_NOTIMPL;
}

/* ISampleGrabber */
static HRESULT WINAPI
SampleGrabber_ISampleGrabber_SetCallback(ISampleGrabber *iface, ISampleGrabberCB *cb, LONG whichMethod)
{
    SG_Impl *This = impl_from_ISampleGrabber(iface);
    TRACE("(%p)->(%p, %u)\n", This, cb, whichMethod);
    if (This->grabberIface)
        ISampleGrabberCB_Release(This->grabberIface);
    This->grabberIface = cb;
    This->grabberMethod = whichMethod;
    if (cb)
        ISampleGrabberCB_AddRef(cb);
    return S_OK;
}

static HRESULT WINAPI SampleGrabber_IMemInputPin_QueryInterface(IMemInputPin *iface, REFIID iid, void **out)
{
    SG_Impl *filter = impl_from_IMemInputPin(iface);
    return IPin_QueryInterface(&filter->sink.pin.IPin_iface, iid, out);
}

static ULONG WINAPI SampleGrabber_IMemInputPin_AddRef(IMemInputPin *iface)
{
    SG_Impl *filter = impl_from_IMemInputPin(iface);
    return IPin_AddRef(&filter->sink.pin.IPin_iface);
}

static ULONG WINAPI SampleGrabber_IMemInputPin_Release(IMemInputPin *iface)
{
    SG_Impl *filter = impl_from_IMemInputPin(iface);
    return IPin_Release(&filter->sink.pin.IPin_iface);
}

/* IMemInputPin */
static HRESULT WINAPI
SampleGrabber_IMemInputPin_GetAllocator(IMemInputPin *iface, IMemAllocator **allocator)
{
    SG_Impl *This = impl_from_IMemInputPin(iface);
    TRACE("(%p)->(%p) allocator = %p\n", This, allocator, This->allocator);
    if (!allocator)
        return E_POINTER;
    *allocator = This->allocator;
    if (!*allocator)
        return VFW_E_NO_ALLOCATOR;
    IMemAllocator_AddRef(*allocator);
    return S_OK;
}

/* IMemInputPin */
static HRESULT WINAPI
SampleGrabber_IMemInputPin_NotifyAllocator(IMemInputPin *iface, IMemAllocator *allocator, BOOL readOnly)
{
    SG_Impl *This = impl_from_IMemInputPin(iface);
    TRACE("(%p)->(%p, %u) allocator = %p\n", This, allocator, readOnly, This->allocator);
    if (This->allocator == allocator)
        return S_OK;
    if (This->allocator)
        IMemAllocator_Release(This->allocator);
    This->allocator = allocator;
    if (allocator)
        IMemAllocator_AddRef(allocator);
    return S_OK;
}

/* IMemInputPin */
static HRESULT WINAPI
SampleGrabber_IMemInputPin_GetAllocatorRequirements(IMemInputPin *iface, ALLOCATOR_PROPERTIES *props)
{
    SG_Impl *This = impl_from_IMemInputPin(iface);
    FIXME("(%p)->(%p): semi-stub\n", This, props);
    if (!props)
        return E_POINTER;
    return This->memOutput ? IMemInputPin_GetAllocatorRequirements(This->memOutput, props) : E_NOTIMPL;
}

/* IMemInputPin */
static HRESULT WINAPI
SampleGrabber_IMemInputPin_Receive(IMemInputPin *iface, IMediaSample *sample)
{
    SG_Impl *This = impl_from_IMemInputPin(iface);
    HRESULT hr;
    TRACE("(%p)->(%p) output = %p, grabber = %p\n", This, sample, This->memOutput, This->grabberIface);
    if (!sample)
        return E_POINTER;
    if (This->oneShot == OneShot_Past)
        return S_FALSE;
    SampleGrabber_callback(This, sample);
    hr = This->memOutput ? IMemInputPin_Receive(This->memOutput, sample) : S_OK;
    if (This->oneShot == OneShot_Wait) {
        This->oneShot = OneShot_Past;
        hr = S_FALSE;
        if (This->source.pin.peer)
            IPin_EndOfStream(This->source.pin.peer);
    }
    return hr;
}

/* IMemInputPin */
static HRESULT WINAPI
SampleGrabber_IMemInputPin_ReceiveMultiple(IMemInputPin *iface, IMediaSample **samples, LONG nSamples, LONG *nProcessed)
{
    SG_Impl *This = impl_from_IMemInputPin(iface);
    LONG idx;
    TRACE("(%p)->(%p, %u, %p) output = %p, grabber = %p\n", This, samples, nSamples, nProcessed, This->memOutput, This->grabberIface);
    if (!samples || !nProcessed)
        return E_POINTER;
    if ((This->filter.state != State_Running) || (This->oneShot == OneShot_Past))
        return S_FALSE;
    for (idx = 0; idx < nSamples; idx++)
        SampleGrabber_callback(This, samples[idx]);
    return This->memOutput ? IMemInputPin_ReceiveMultiple(This->memOutput, samples, nSamples, nProcessed) : S_OK;
}

/* IMemInputPin */
static HRESULT WINAPI
SampleGrabber_IMemInputPin_ReceiveCanBlock(IMemInputPin *iface)
{
    SG_Impl *This = impl_from_IMemInputPin(iface);
    TRACE("(%p)\n", This);
    return This->memOutput ? IMemInputPin_ReceiveCanBlock(This->memOutput) : S_OK;
}

/* IPin - input pin */
static HRESULT WINAPI
SampleGrabber_In_IPin_ReceiveConnection(IPin *iface, IPin *connector, const AM_MEDIA_TYPE *type)
{
    SG_Impl *filter = CONTAINING_RECORD(iface, SG_Impl, sink.pin.IPin_iface);

    TRACE("filter %p, connector %p, type %p.\n", filter, connector, type);

    if (!connector)
        return E_POINTER;
    if (filter->sink.pin.peer)
        return VFW_E_ALREADY_CONNECTED;
    if (filter->filter.state != State_Stopped)
        return VFW_E_NOT_STOPPED;
    if (type) {
	TRACE("Media type: %s/%s ssize: %u format: %s (%u bytes)\n",
	    debugstr_guid(&type->majortype), debugstr_guid(&type->subtype),
	    type->lSampleSize,
	    debugstr_guid(&type->formattype), type->cbFormat);
	if (!IsEqualGUID(&type->formattype, &FORMAT_None) &&
	    !IsEqualGUID(&type->formattype, &GUID_NULL) &&
	    !type->pbFormat)
	    return VFW_E_INVALIDMEDIATYPE;
	if (!IsEqualGUID(&filter->mtype.majortype,&GUID_NULL) &&
	    !IsEqualGUID(&filter->mtype.majortype,&type->majortype))
	    return VFW_E_TYPE_NOT_ACCEPTED;
	if (!IsEqualGUID(&filter->mtype.subtype,&MEDIASUBTYPE_None) &&
	    !IsEqualGUID(&filter->mtype.subtype,&type->subtype))
	    return VFW_E_TYPE_NOT_ACCEPTED;
	if (!IsEqualGUID(&filter->mtype.formattype,&GUID_NULL) &&
	    !IsEqualGUID(&filter->mtype.formattype,&FORMAT_None) &&
	    !IsEqualGUID(&filter->mtype.formattype,&type->formattype))
	    return VFW_E_TYPE_NOT_ACCEPTED;

        FreeMediaType(&filter->mtype);
        CopyMediaType(&filter->mtype, type);
        CopyMediaType(&filter->sink.pin.mt, type);
    }
    IPin_AddRef(filter->sink.pin.peer = connector);

    return S_OK;
}

static const ISampleGrabberVtbl ISampleGrabber_VTable =
{
    SampleGrabber_ISampleGrabber_QueryInterface,
    SampleGrabber_ISampleGrabber_AddRef,
    SampleGrabber_ISampleGrabber_Release,
    SampleGrabber_ISampleGrabber_SetOneShot,
    SampleGrabber_ISampleGrabber_SetMediaType,
    SampleGrabber_ISampleGrabber_GetConnectedMediaType,
    SampleGrabber_ISampleGrabber_SetBufferSamples,
    SampleGrabber_ISampleGrabber_GetCurrentBuffer,
    SampleGrabber_ISampleGrabber_GetCurrentSample,
    SampleGrabber_ISampleGrabber_SetCallback,
};

static const IMemInputPinVtbl IMemInputPin_VTable =
{
    SampleGrabber_IMemInputPin_QueryInterface,
    SampleGrabber_IMemInputPin_AddRef,
    SampleGrabber_IMemInputPin_Release,
    SampleGrabber_IMemInputPin_GetAllocator,
    SampleGrabber_IMemInputPin_NotifyAllocator,
    SampleGrabber_IMemInputPin_GetAllocatorRequirements,
    SampleGrabber_IMemInputPin_Receive,
    SampleGrabber_IMemInputPin_ReceiveMultiple,
    SampleGrabber_IMemInputPin_ReceiveCanBlock,
};

static const IPinVtbl sink_vtbl =
{
    BasePinImpl_QueryInterface,
    BasePinImpl_AddRef,
    BasePinImpl_Release,
    BaseInputPinImpl_Connect,
    SampleGrabber_In_IPin_ReceiveConnection,
    BasePinImpl_Disconnect,
    BasePinImpl_ConnectedTo,
    BasePinImpl_ConnectionMediaType,
    BasePinImpl_QueryPinInfo,
    BasePinImpl_QueryDirection,
    BasePinImpl_QueryId,
    BasePinImpl_QueryAccept,
    BasePinImpl_EnumMediaTypes,
    BasePinImpl_QueryInternalConnections,
    BaseInputPinImpl_EndOfStream,
    BaseInputPinImpl_BeginFlush,
    BaseInputPinImpl_EndFlush,
    BaseInputPinImpl_NewSegment
};

static inline SG_Impl *impl_from_sink_pin(struct strmbase_pin *iface)
{
    return CONTAINING_RECORD(iface, SG_Impl, sink.pin);
}

static HRESULT sample_grabber_sink_query_interface(struct strmbase_pin *iface, REFIID iid, void **out)
{
    SG_Impl *filter = impl_from_sink_pin(iface);

    if (IsEqualGUID(iid, &IID_IMemInputPin))
        *out = &filter->IMemInputPin_iface;
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static HRESULT sample_grabber_sink_query_accept(struct strmbase_pin *iface, const AM_MEDIA_TYPE *mt)
{
    return S_OK;
}

static HRESULT sample_grabber_sink_get_media_type(struct strmbase_pin *iface,
        unsigned int index, AM_MEDIA_TYPE *mt)
{
    SG_Impl *filter = impl_from_sink_pin(iface);

    if (!index)
    {
        CopyMediaType(mt, &filter->mtype);
        return S_OK;
    }
    return VFW_S_NO_MORE_ITEMS;
}

static const struct strmbase_sink_ops sink_ops =
{
    .base.pin_query_interface = sample_grabber_sink_query_interface,
    .base.pin_query_accept = sample_grabber_sink_query_accept,
    .base.pin_get_media_type = sample_grabber_sink_get_media_type,
};

static inline SG_Impl *impl_from_source_pin(struct strmbase_pin *iface)
{
    return CONTAINING_RECORD(iface, SG_Impl, source.pin);
}

static HRESULT sample_grabber_source_query_interface(struct strmbase_pin *iface, REFIID iid, void **out)
{
    SG_Impl *filter = impl_from_source_pin(iface);

    if (IsEqualGUID(iid, &IID_IMediaPosition) || IsEqualGUID(iid, &IID_IMediaSeeking))
        return IUnknown_QueryInterface(filter->seekthru_unk, iid, out);
    else
        return E_NOINTERFACE;
}

static HRESULT sample_grabber_source_query_accept(struct strmbase_pin *iface, const AM_MEDIA_TYPE *mt)
{
    return S_OK;
}

static HRESULT sample_grabber_source_get_media_type(struct strmbase_pin *iface,
        unsigned int index, AM_MEDIA_TYPE *mt)
{
    SG_Impl *filter = impl_from_source_pin(iface);

    if (!index)
    {
        CopyMediaType(mt, &filter->mtype);
        return S_OK;
    }
    return VFW_S_NO_MORE_ITEMS;
}

static HRESULT WINAPI sample_grabber_source_AttemptConnection(struct strmbase_source *iface,
        IPin *peer, const AM_MEDIA_TYPE *mt)
{
    SG_Impl *filter = impl_from_source_pin(&iface->pin);
    HRESULT hr;

    if (filter->source.pin.peer)
        return VFW_E_ALREADY_CONNECTED;
    if (filter->filter.state != State_Stopped)
        return VFW_E_NOT_STOPPED;
    if (!IsEqualGUID(&mt->majortype, &filter->mtype.majortype))
        return VFW_E_TYPE_NOT_ACCEPTED;
    if (!IsEqualGUID(&mt->subtype, &filter->mtype.subtype))
        return VFW_E_TYPE_NOT_ACCEPTED;
    if (!IsEqualGUID(&mt->formattype, &FORMAT_None)
            && !IsEqualGUID(&mt->formattype, &GUID_NULL)
            && !IsEqualGUID(&mt->formattype, &filter->mtype.majortype))
        return VFW_E_TYPE_NOT_ACCEPTED;
    if (!IsEqualGUID(&mt->formattype, &FORMAT_None)
            && !IsEqualGUID(&mt->formattype, &GUID_NULL)
            && !mt->pbFormat)
        return VFW_E_TYPE_NOT_ACCEPTED;

    IPin_AddRef(filter->source.pin.peer = peer);
    CopyMediaType(&filter->source.pin.mt, mt);
    if (SUCCEEDED(hr = IPin_ReceiveConnection(peer, &filter->source.pin.IPin_iface, mt)))
        hr = IPin_QueryInterface(peer, &IID_IMemInputPin, (void **)&filter->source.pMemInputPin);

    if (FAILED(hr))
    {
        IPin_Release(filter->source.pin.peer);
        filter->source.pin.peer = NULL;
        FreeMediaType(&filter->source.pin.mt);
    }

    return hr;
}

static const struct strmbase_source_ops source_ops =
{
    .base.pin_query_interface = sample_grabber_source_query_interface,
    .base.pin_query_accept = sample_grabber_source_query_accept,
    .base.pin_get_media_type = sample_grabber_source_get_media_type,
    .pfnAttemptConnection = sample_grabber_source_AttemptConnection,
};

static const IPinVtbl source_vtbl =
{
    BasePinImpl_QueryInterface,
    BasePinImpl_AddRef,
    BasePinImpl_Release,
    BaseOutputPinImpl_Connect,
    BaseOutputPinImpl_ReceiveConnection,
    BaseOutputPinImpl_Disconnect,
    BasePinImpl_ConnectedTo,
    BasePinImpl_ConnectionMediaType,
    BasePinImpl_QueryPinInfo,
    BasePinImpl_QueryDirection,
    BasePinImpl_QueryId,
    BasePinImpl_QueryAccept,
    BasePinImpl_EnumMediaTypes,
    BasePinImpl_QueryInternalConnections,
    BaseOutputPinImpl_EndOfStream,
    BaseOutputPinImpl_BeginFlush,
    BaseOutputPinImpl_EndFlush,
    BasePinImpl_NewSegment,
};

HRESULT SampleGrabber_create(IUnknown *outer, void **out)
{
    SG_Impl* obj = NULL;
    ISeekingPassThru *passthru;
    HRESULT hr;

    obj = CoTaskMemAlloc(sizeof(SG_Impl));
    if (NULL == obj) {
        *out = NULL;
        return E_OUTOFMEMORY;
    }
    ZeroMemory(obj, sizeof(SG_Impl));

    strmbase_filter_init(&obj->filter, outer, &CLSID_SampleGrabber, &filter_ops);
    obj->ISampleGrabber_iface.lpVtbl = &ISampleGrabber_VTable;
    obj->IMemInputPin_iface.lpVtbl = &IMemInputPin_VTable;

    strmbase_sink_init(&obj->sink, &sink_vtbl, &obj->filter, L"In", &sink_ops, NULL);
    strmbase_source_init(&obj->source, &source_vtbl, &obj->filter, L"Out", &source_ops);

    obj->mtype.majortype = GUID_NULL;
    obj->mtype.subtype = MEDIASUBTYPE_None;
    obj->mtype.formattype = FORMAT_None;
    obj->allocator = NULL;
    obj->memOutput = NULL;
    obj->grabberIface = NULL;
    obj->grabberMethod = -1;
    obj->oneShot = OneShot_None;
    obj->bufferLen = -1;
    obj->bufferData = NULL;

    hr = CoCreateInstance(&CLSID_SeekingPassThru, &obj->filter.IUnknown_inner,
            CLSCTX_INPROC_SERVER, &IID_IUnknown, (void **)&obj->seekthru_unk);
    if(hr)
        return hr;
    IUnknown_QueryInterface(obj->seekthru_unk, &IID_ISeekingPassThru, (void**)&passthru);
    ISeekingPassThru_Init(passthru, FALSE, &obj->sink.pin.IPin_iface);
    ISeekingPassThru_Release(passthru);

    *out = &obj->filter.IUnknown_inner;
    return S_OK;
}
