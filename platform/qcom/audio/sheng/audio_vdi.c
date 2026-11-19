/* SPDX-License-Identifier: MIT
 * Donor6.1 VDI playback endpoint. Real PCM/volume operations use the isolated
 * stock303 PAL process. Input is advertised only after the backend capability gate.
 */
#include "iaudio_manager_vdi.h"
#include "pal_client.h"
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

_Static_assert(offsetof(struct IAudioRenderVdi,SetVolumeWithRamp)==120,"donor ramp slot");
_Static_assert(offsetof(struct IAudioRenderVdi,Start)==256,"donor start slot");
_Static_assert(offsetof(struct IAudioAdapterVdi,GetPortCapability)==40,"donor capability slot");
_Static_assert(sizeof(struct AudioAdapterDescriptorVdi)==24,"donor descriptors");
_Static_assert(sizeof(struct AudioSampleAttributesVdi)==168,"donor sample attributes");

/* asteroids: `running` is what audio_server asked for (Start..Stop); `fd`, `backend_open` and `started` are the PAL
 * server's side, which can be lost under it: a request that times out (the phone dozing mid-request, a slow stop)
 * leaves us unable to tell what the server did, and the server can be restarted. Then the connection is dropped (the
 * server closes the stream with it) and the next write reconnects, reopens and restarts the stream. */
struct render {
    struct IAudioRenderVdi api;
    pthread_mutex_t lock;
    int fd, running, muted, backend_open, started;
    float volume;
    uint64_t write_calls;
    struct AudioSampleAttributesVdi attrs;
};
struct adapter {
    struct IAudioAdapterVdi api;
    unsigned renders;
    unsigned captures;
};

static int unsupported(void *self,...){(void)self; return -ENOTSUP;}
static void link_lost(struct render *r,unsigned op,int rc)
{
    if (r->fd>=0) close(r->fd);
    r->fd=-1; r->backend_open=0; r->started=0;
    fprintf(stderr,"SHENG_AUDIO_LINK_LOST op=%u rc=%d\n",op,rc);
}
/* Caller holds r->lock. */
static int call(struct render *r,unsigned op,unsigned arg0,unsigned arg1,const void *pcm,unsigned bytes,
    struct sheng_pal_reply *reply)
{
    if (r->fd<0) {
        r->fd=sheng_pal_connect();
        if (r->fd<0) { int rc=r->fd; r->fd=-1; return rc; }
    }
    int broken=0, rc=sheng_pal_call(r->fd,op,arg0,arg1,pcm,bytes,reply,&broken);
    if (broken) link_lost(r,op,rc);
    return rc;
}
static int command(struct render *r,unsigned op,unsigned arg)
{
    struct sheng_pal_reply reply={0};
    return call(r,op,arg,0,0,0,&reply);
}
static int backend_ready(void)
{
    int fd=sheng_pal_connect();
    if (fd<0) return fd;
    struct sheng_pal_reply reply={0};
    int rc=sheng_pal_request(fd,SHENG_PAL_STATUS,0,0,0,0,&reply);
    close(fd);
    return rc;
}
/* Caller holds r->lock. A PAL stopped handle can accept writes while its
 * speaker route stays silent. Release that handle and open a fresh real stream.
 * Preserve the native sink object and its requested volume/mute state. */
static int open_backend(struct render *r)
{
    if (r->backend_open) return 0;
    struct sheng_pal_reply reply={0};
    int rc=call(r,SHENG_PAL_OPEN,48000,2,0,0,&reply);
    if (rc==-EBUSY) {  /* the server still has this connection's last stream */
        (void)command(r,SHENG_PAL_CLOSE,0);
        rc=call(r,SHENG_PAL_OPEN,48000,2,0,0,&reply);
    }
    if (rc) return rc;
    r->backend_open=1; r->started=0;
    rc=command(r,SHENG_PAL_VOLUME,r->muted?0:(unsigned)(r->volume*1000000.0f));
    fprintf(stderr,"SHENG_AUDIO_BACKEND_REOPEN rc=%d volume=%.4f muted=%d\n",rc,r->volume,r->muted);
    return rc;
}
static int start_backend(struct render *r)
{
    int rc=open_backend(r);
    if (!rc && !r->started) rc=command(r,SHENG_PAL_START,0);
    if (!rc) r->started=1;
    return rc;
}
/* A lost link counts as released: the server closed the stream when the connection went. */
static int stop_backend(struct render *r)
{
    r->running=0;
    if (!r->backend_open) return 0;
    int rc=command(r,SHENG_PAL_STOP,0);
    if (r->fd<0) return 0;
    r->started=0;
    int crc=command(r,SHENG_PAL_CLOSE,0);
    if (r->fd<0) return 0;
    if (!crc) r->backend_open=0;
    fprintf(stderr,"SHENG_AUDIO_BACKEND_RELEASE stop=%d close=%d\n",rc,crc);
    return crc;
}
static int start(struct IAudioRenderVdi *self)
{
    struct render *r=(void *)self; pthread_mutex_lock(&r->lock);
    int rc=start_backend(r);
    if (rc && r->fd<0) rc=start_backend(r);  /* the link was stale: once more on a new one */
    if (!rc) r->running=1;
    pthread_mutex_unlock(&r->lock); return rc;
}
static int stop(struct IAudioRenderVdi *self)
{
    struct render *r=(void *)self; pthread_mutex_lock(&r->lock);
    int rc=stop_backend(r);
    pthread_mutex_unlock(&r->lock); return rc;
}
static int pause_render(struct IAudioRenderVdi *self)
{
    return stop(self);
}
static int resume_render(struct IAudioRenderVdi *self)
{
    return start(self);
}
static int flush(struct IAudioRenderVdi *self)
{
    struct render *r=(void *)self; pthread_mutex_lock(&r->lock);
    int was_running=r->running, rc=stop_backend(r);
    if (!rc) rc=was_running?start_backend(r):open_backend(r);
    if (!rc) r->running=was_running;
    pthread_mutex_unlock(&r->lock); return rc;
}
static int set_volume(struct IAudioRenderVdi *self,float volume)
{
    if (!(volume>=0 && volume<=1)) return -EINVAL;
    struct render *r=(void *)self; pthread_mutex_lock(&r->lock);
    int rc=r->backend_open?command(r,SHENG_PAL_VOLUME,r->muted?0:(unsigned)(volume*1000000.0f)):0;
    if (!rc || r->fd<0) { r->volume=volume; rc=0; }  /* a new stream gets it when it opens */
    pthread_mutex_unlock(&r->lock); return rc;
}
static int set_ramp(struct IAudioRenderVdi *self,float volume,uint32_t duration)
{
    return duration ? -ENOTSUP : set_volume(self,volume);
}
static int get_volume(struct IAudioRenderVdi *self,float *volume)
{
    if (!volume) return -EINVAL;
    struct render *r=(void *)self; pthread_mutex_lock(&r->lock);
    *volume=r->volume; pthread_mutex_unlock(&r->lock); return 0;
}
static int set_mute(struct IAudioRenderVdi *self,bool mute)
{
    struct render *r=(void *)self; pthread_mutex_lock(&r->lock);
    int rc=r->backend_open?command(r,SHENG_PAL_VOLUME,mute?0:(unsigned)(r->volume*1000000.0f)):0;
    if (!rc || r->fd<0) { r->muted=mute; rc=0; }
    pthread_mutex_unlock(&r->lock); return rc;
}
static int get_mute(struct IAudioRenderVdi *self,bool *mute)
{
    if (!mute) return -EINVAL;
    struct render *r=(void *)self; pthread_mutex_lock(&r->lock);
    *mute=r->muted; pthread_mutex_unlock(&r->lock); return 0;
}
static int render_frame(struct IAudioRenderVdi *self,const int8_t *data,uint32_t len,uint64_t *written)
{
    if (!data || !written) return -EINVAL;
    *written=0;
    struct render *r=(void *)self;
    unsigned sample_bytes=r->attrs.format==AUDIO_VDI_FORMAT_TYPE_PCM_32_BIT?4:2;
    if (len%(sample_bytes*2)) return -EINVAL;
    pthread_mutex_lock(&r->lock);
    int rc=0;
    while (*written<len) {
        unsigned char pcm[SHENG_PAL_MAX_DATA];
        unsigned input=len-(unsigned)*written;
        unsigned max_input=SHENG_PAL_MAX_DATA*(sample_bytes/2);
        if (input>max_input) input=max_input;
        const unsigned char *p=(const unsigned char *)data+*written;
        unsigned output=input;
        const void *payload=p;
        if (sample_bytes==4) {
            output=input/2;
            for (unsigned i=0;i<output/2;i++) {
                /* Signed little-endian S32 -> S16, preserving the high bits. */
                pcm[2*i]=p[4*i+2]; pcm[2*i+1]=p[4*i+3];
            }
            payload=pcm;
        }
        r->write_calls++;
        if (r->write_calls==1 || r->write_calls==2 || r->write_calls==8 || r->write_calls==32) {
            unsigned peak=0,nonzero=0;
            const unsigned char *samples=payload;
            for (unsigned i=0;i<output;i+=2) {
                int v=(int16_t)((unsigned)samples[i]|((unsigned)samples[i+1]<<8));
                unsigned mag=(unsigned)(v<0?-v:v);
                if(mag) nonzero++;
                if(mag>peak) peak=mag;
            }
            fprintf(stderr,"SHENG_AUDIO_PCM call=%llu input_bytes=%u output_bytes=%u peak_s16=%u nonzero=%u volume=%.4f muted=%d\n",
                (unsigned long long)r->write_calls,input,output,peak,nonzero,r->volume,r->muted);
        }
        struct sheng_pal_reply reply={0};
        /* Audio_server writes only between Start and Stop; if the server's stream isn't running then (the link
         * was lost, or the server was restarted), bring it back and write again, once per call. */
        for (int attempt=0;;attempt++) {
            rc=r->started?call(r,SHENG_PAL_WRITE,0,0,payload,output,&reply):-ENOTCONN;
            if (!rc || attempt || (r->started && rc!=-EINVAL && rc!=-ENODEV && r->fd>=0)) break;
            if (r->backend_open && r->fd>=0) { (void)command(r,SHENG_PAL_CLOSE,0); r->backend_open=0; r->started=0; }
            int src=start_backend(r);
            if (src && r->fd<0) src=start_backend(r);
            fprintf(stderr,"SHENG_AUDIO_RECOVER write_rc=%d start_rc=%d\n",rc,src);
            if (src) { rc=src; break; }
            r->running=1;
        }
        *written+=(uint64_t)reply.bytes*(sample_bytes/2);
        if (rc || reply.bytes!=output) { if (!rc) rc=-EIO; break; }
    }
    pthread_mutex_unlock(&r->lock);
    return rc;
}
static int get_size(struct IAudioRenderVdi *self,uint64_t *size)
{
    if (!size) return -EINVAL;
    struct render *r=(void *)self;
    *size=r->attrs.format==AUDIO_VDI_FORMAT_TYPE_PCM_32_BIT?8:4; return 0;
}
static int get_count(struct IAudioRenderVdi *self,uint64_t *count)
{ (void)self; if (!count) return -EINVAL; *count=240; return 0; }
static int get_buffer_size(struct IAudioRenderVdi *self,uint64_t *bytes)
{ int rc=get_size(self,bytes); if (!rc) *bytes*=240; return rc; }
static int get_attrs(struct IAudioRenderVdi *self,struct AudioSampleAttributesVdi *attrs)
{ if (!attrs) return -EINVAL; *attrs=((struct render *)self)->attrs; return 0; }
static int no_drain(struct IAudioRenderVdi *self,bool *supported)
{ (void)self; if (!supported) return -EINVAL; *supported=false; return 0; }
static int no_pause(struct IAudioRenderVdi *self,bool *pause,bool *resume)
{ (void)self; if (!pause || !resume) return -EINVAL; *pause=false; *resume=false; return 0; }
/* asteroids: call audio. The modem's voice stream lives in the PAL server (SHENG_PAL_VOICE_*, opened by
 * asteroids_atd while a call is up); audio_server's choice of output device for the call reaches us as a scene and
 * as routes, and the voice stream follows it: speaker with the speaker mic, or earpiece with the handset mic. */
static void voice_request(unsigned op,unsigned arg)
{
    int fd=sheng_pal_connect();
    if (fd<0) return;
    struct sheng_pal_reply reply={0};
    int rc=sheng_pal_request(fd,op,arg,0,0,0,&reply);
    close(fd);
    fprintf(stderr,"SHENG_VOICE op=%u arg=0x%x rc=%d\n",op,arg,rc);
}
static void voice_follow(enum AudioPortPinVdi pin)
{
    /* PAL device ids: earpiece 2, speaker 3, handset mic 27, speaker mic 28. */
    if (pin==PIN_VDI_OUT_SPEAKER) voice_request(SHENG_PAL_VOICE_ROUTE,3|28u<<16);
    else if (pin==PIN_VDI_OUT_EARPIECE) voice_request(SHENG_PAL_VOICE_ROUTE,2|27u<<16);
}
static int scene_supported(struct IAudioRenderVdi *self,const struct AudioSceneDescriptorVdi *scene,bool *supported)
{ (void)self; if (!scene || !supported) return -EINVAL; *supported=true; return 0; }
static int select_scene(struct IAudioRenderVdi *self,const struct AudioSceneDescriptorVdi *scene)
{
    (void)self;
    if (!scene) return -EINVAL;
    fprintf(stderr,"SHENG_SCENE id=%u pins=0x%x\n",scene->scene.id,(unsigned)scene->desc.pins);
    voice_follow(scene->desc.pins);
    return 0;
}

static int create_render(struct IAudioAdapterVdi *self,const struct AudioDeviceDescriptorVdi *desc,
    const struct AudioSampleAttributesVdi *attrs,struct IAudioRenderVdi **out)
{
    if (!desc || !attrs || !out) return -EINVAL;
    *out=0;
    fprintf(stderr,"SHENG_AUDIO_CREATE pins=%u type=%u rate=%u channels=%u format=%u\n",
        (unsigned)desc->pins,(unsigned)attrs->type,attrs->sampleRate,attrs->channelCount,(unsigned)attrs->format);
    if (desc->pins!=PIN_VDI_OUT_SPEAKER || attrs->sampleRate!=48000 || attrs->channelCount!=2 ||
        !attrs->interleaved || attrs->isBigEndian ||
        (attrs->type!=AUDIO_VDI_IN_MEDIA && attrs->type!=AUDIO_VDI_IN_RINGTONE && attrs->type!=AUDIO_VDI_IN_COMMUNICATION) ||
        (attrs->format!=AUDIO_VDI_FORMAT_TYPE_PCM_16_BIT && attrs->format!=AUDIO_VDI_FORMAT_TYPE_PCM_32_BIT)) return -ENOTSUP;
    struct render *r=calloc(1,sizeof(*r));
    if (!r) return -ENOMEM;
    /* asteroids: full scale, as Android runs PAL; audio_server applies the volume in software (sheng: 0.03). */
    r->fd=-1; r->attrs=*attrs; r->volume=1.0f;
    int rc=open_backend(r);
    if (rc) { if (r->fd>=0) close(r->fd); free(r); return rc; }
    rc=pthread_mutex_init(&r->lock,0);
    if (rc) { (void)command(r,SHENG_PAL_CLOSE,0); close(r->fd); free(r); return -rc; }
    /* Every optional method returns a real unsupported error until implemented. */
#define OPTIONAL(field) r->api.field=(__typeof__(r->api.field))unsupported
    OPTIONAL(GetLatency); OPTIONAL(GetRenderPosition); OPTIONAL(SetRenderSpeed); OPTIONAL(GetRenderSpeed);
    OPTIONAL(SetChannelMode); OPTIONAL(GetChannelMode); OPTIONAL(RegCallback); OPTIONAL(DrainBuffer);
    OPTIONAL(GetGainThreshold); OPTIONAL(GetGain); OPTIONAL(SetGain); OPTIONAL(SetSampleAttributes);
    OPTIONAL(GetCurrentChannelId); OPTIONAL(SetExtraParams); OPTIONAL(GetExtraParams);
    OPTIONAL(ReqMmapBuffer); OPTIONAL(GetMmapPosition); OPTIONAL(AddAudioEffect); OPTIONAL(RemoveAudioEffect);
    OPTIONAL(AudioDevDump); OPTIONAL(SetBufferSize);
#undef OPTIONAL
    r->api.RenderFrame=render_frame; r->api.Start=start; r->api.Stop=stop;
    r->api.Pause=pause_render; r->api.Resume=resume_render; r->api.Flush=flush; r->api.TurnStandbyMode=stop;
    r->api.SetVolume=set_volume; r->api.GetVolume=get_volume; r->api.SetVolumeWithRamp=set_ramp;
    r->api.SetMute=set_mute; r->api.GetMute=get_mute;
    r->api.GetFrameSize=get_size; r->api.GetFrameCount=get_count; r->api.GetFrameBufferSize=get_buffer_size;
    r->api.GetSampleAttributes=get_attrs; r->api.IsSupportsDrain=no_drain;
    r->api.IsSupportsPauseAndResume=no_pause;
    r->api.CheckSceneCapability=scene_supported; r->api.SelectScene=select_scene;
    ((struct adapter *)self)->renders++;
    *out=&r->api;
    return 0;
}
static int destroy_render(struct IAudioAdapterVdi *self,struct IAudioRenderVdi *render)
{
    if (!render) return -EINVAL;
    struct render *r=(void *)render;
    pthread_mutex_lock(&r->lock);
    int rc=r->backend_open?command(r,SHENG_PAL_CLOSE,0):0; if (r->fd>=0) close(r->fd); r->fd=-1;
    pthread_mutex_unlock(&r->lock); pthread_mutex_destroy(&r->lock); free(r);
    if (((struct adapter *)self)->renders) ((struct adapter *)self)->renders--;
    return rc;
}
static int init_ports(struct IAudioAdapterVdi *self){(void)self; return backend_ready();}
static void log_route_node(const char *what,const struct AudioRouteNodeVdi *n)
{
    if (n->type==AUDIO_VDI_PORT_DEVICE_TYPE)
        fprintf(stderr,"SHENG_ROUTE %s port=%d device pin=0x%x desc=%s\n",what,n->portId,(unsigned)n->ext.device.type,
            n->ext.device.desc?n->ext.device.desc:"");
    else if (n->type==AUDIO_VDI_PORT_MIX_TYPE)
        fprintf(stderr,"SHENG_ROUTE %s port=%d mix stream=%d source=%d\n",what,n->portId,n->ext.mix.streamId,n->ext.mix.source);
    else
        fprintf(stderr,"SHENG_ROUTE %s port=%d type=%d\n",what,n->portId,(int)n->type);
}
static int update_route(struct IAudioAdapterVdi *self,const struct AudioRouteVdi *route,int32_t *handle)
{
    static int32_t next;
    (void)self;
    if (!route) return -EINVAL;
    for (uint32_t i=0;i<route->sourcesLen;i++) log_route_node("source",&route->sources[i]);
    for (uint32_t i=0;i<route->sinksLen;i++) {
        log_route_node("sink",&route->sinks[i]);
        if (route->sinks[i].type==AUDIO_VDI_PORT_DEVICE_TYPE) voice_follow(route->sinks[i].ext.device.type);
    }
    if (handle) *handle=++next;
    return 0;
}
static int release_route(struct IAudioAdapterVdi *self,int32_t handle)
{ (void)self; fprintf(stderr,"SHENG_ROUTE release %d\n",handle); return 0; }
static int set_voice_volume(struct IAudioAdapterVdi *self,float volume)
{
    (void)self;
    if (!(volume>=0 && volume<=1)) return -EINVAL;
    voice_request(SHENG_PAL_VOICE_VOLUME,(unsigned)(volume*1000000.0f));
    return 0;
}
static int adapter_mic_mute(struct IAudioAdapterVdi *self, bool mute);
static int voice_mic_mute(struct IAudioAdapterVdi *self,bool mute)
{
    /* Mute covers the call's uplink too (the voice stream's own mute, as Android's AudioVoice). */
    voice_request(SHENG_PAL_VOICE_MUTE,mute);
    return adapter_mic_mute(self,mute);
}
static int capability(struct IAudioAdapterVdi *self,const struct AudioPortVdi *port,
    struct AudioPortCapabilityVdi *cap)
{
    (void)self;
    if (!port || !cap || port->dir!=PORT_VDI_OUT) return -EINVAL;
    memset(cap,0,sizeof(*cap));
    cap->formats=calloc(2,sizeof(*cap->formats));
    cap->subPorts=calloc(1,sizeof(*cap->subPorts));
    cap->supportSampleFormats=calloc(2,sizeof(*cap->supportSampleFormats));
    if (!cap->formats || !cap->subPorts || !cap->supportSampleFormats) {
        free(cap->formats); free(cap->subPorts); free(cap->supportSampleFormats); memset(cap,0,sizeof(*cap)); return -ENOMEM;
    }
    cap->subPorts[0].desc=strdup("Sheng stock303 speaker");
    if (!cap->subPorts[0].desc) {
        free(cap->formats); free(cap->subPorts); free(cap->supportSampleFormats); memset(cap,0,sizeof(*cap)); return -ENOMEM;
    }
    cap->deviceId=port->portId; cap->hardwareMode=true; cap->formatNum=2; cap->formatsLen=2;
    cap->formats[0]=AUDIO_VDI_FORMAT_TYPE_PCM_16_BIT; cap->formats[1]=AUDIO_VDI_FORMAT_TYPE_PCM_32_BIT;
    cap->sampleRateMasks=AUDIO_VDI_SAMPLE_RATE_MASK_48000;
    cap->channelMasks=AUDIO_VDI_CHANNEL_STEREO; cap->channelCount=2;
    cap->subPortsLen=1; cap->subPorts[0].portId=port->portId; cap->subPorts[0].mask=PORT_VDI_PASSTHROUGH_LPCM;
    cap->supportSampleFormatsLen=2; cap->supportSampleFormats[0]=AUDIO_VDI_SAMPLE_FORMAT_S16;
    cap->supportSampleFormats[1]=AUDIO_VDI_SAMPLE_FORMAT_S32;
    return 0;
}
static int get_extra(struct IAudioAdapterVdi *self,enum AudioExtParamKeyVdi key,
    const char *condition,char *value,uint32_t len)
{
    (void)self;
    fprintf(stderr,"SHENG_AUDIO_GET_EXTRA key=%u condition=%s\n",(unsigned)key,condition?condition:"(null)");
    if (value && len) value[0]=0;
    return -ENOTSUP;
}
#include "audio_capture_vdi.inc"
static int load_adapter(struct IAudioManagerVdi *self,const struct AudioAdapterDescriptorVdi *desc,
    struct IAudioAdapterVdi **out)
{
    (void)self;
    if (!desc || !desc->adapterName || !out || strcmp(desc->adapterName,"primary")) return -EINVAL;
    *out=0; int rc=backend_ready();
    fprintf(stderr,"SHENG_MIC_LOAD backend_status=%d\n",rc);
    if (rc) return rc;
    struct adapter *a=calloc(1,sizeof(*a)); if (!a) return -ENOMEM;
#define OPTIONAL(field) a->api.field=(__typeof__(a->api.field))unsupported
    OPTIONAL(SetPassthroughMode); OPTIONAL(GetPassthroughMode);
    OPTIONAL(GetDeviceStatus); OPTIONAL(SetExtraParams);
#undef OPTIONAL
    for (unsigned i=0;i<sizeof(a->api.donorExtensions)/sizeof(a->api.donorExtensions[0]);i++) a->api.donorExtensions[i]=unsupported;
    a->api.InitAllPorts=init_ports; a->api.CreateRender=create_render; a->api.DestroyRender=destroy_render;
    a->api.GetPortCapability=capture_capability; a->api.GetExtraParams=get_extra;
    a->api.CreateCapture=create_capture; a->api.DestroyCapture=destroy_capture;
    a->api.SetMicMute=voice_mic_mute; a->api.GetMicMute=adapter_mic_get_mute;
    a->api.UpdateAudioRoute=update_route; a->api.ReleaseAudioRoute=release_route;
    a->api.SetVoiceVolume=set_voice_volume;
    *out=&a->api; return 0;
}
static int unload_adapter(struct IAudioManagerVdi *self,struct IAudioAdapterVdi *adapter)
{
    (void)self;
    if (!adapter) return -EINVAL;
    if (((struct adapter *)adapter)->renders ||
        __atomic_load_n(&((struct adapter *)adapter)->captures,__ATOMIC_ACQUIRE)) return -EBUSY;
    free(adapter);
    return 0;
}
static int get_adapters(struct IAudioManagerVdi *self,struct AudioAdapterDescriptorVdi *out,uint32_t *count)
{
    (void)self;
    if (!out || !count || !*count) return -EINVAL;
    int rc=backend_ready(); if (rc) { *count=0; return rc; }
    char *name=strdup("primary"), *port_name=strdup("speaker");
    struct AudioPortVdi *ports=calloc(1,sizeof(*ports));
    if (!name || !port_name || !ports) { free(name); free(port_name); free(ports); return -ENOMEM; }
    *ports=(struct AudioPortVdi){PORT_VDI_OUT,0,port_name};
    unsigned port_count=1;
    int capture_rc=capture_available();
    fprintf(stderr,"SHENG_MIC_ENUM capture_status=%d\n",capture_rc);
    if (!capture_rc) {
        struct AudioPortVdi *both=realloc(ports,2*sizeof(*ports));
        char *mic=strdup("microphone");
        if (!both || !mic) {
            free(mic); free(name); free(port_name); free(both?both:ports); return -ENOMEM;
        }
        ports=both; ports[1]=(struct AudioPortVdi){PORT_VDI_IN,1,mic}; port_count=2;
    }
    *out=(struct AudioAdapterDescriptorVdi){name,ports,port_count}; *count=1;
    fprintf(stderr,"SHENG_MIC_ENUM adapters=1 ports=%u\n",port_count);
    return 0;
}
static int release_manager(struct IAudioManagerVdi *self){(void)self; return 0;}
struct IAudioManagerVdi *AudioManagerCreateIfInstance(void)
{
    static struct IAudioManagerVdi manager={get_adapters,load_adapter,unload_adapter,release_manager};
    return &manager;
}
