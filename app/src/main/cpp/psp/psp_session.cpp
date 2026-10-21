// SPDX-License-Identifier: GPL-2.0-or-later
#include "psp_session.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <libretro.h>
#include <android/log.h>
#include <dlfcn.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

// Exact exported ABI of the pinned libretro/LibretroGraphicsContext.h. Do not
// write this state or reinterpret it as int: only upstream owns its transitions.
namespace Libretro {
enum class EmuThreadState { DISABLED, START_REQUESTED, RUNNING, PAUSE_REQUESTED, PAUSED, QUIT_REQUESTED, STOPPED };
}

namespace emuorbit::psp {
namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
template<class T>T symbol(void* library,const char* name){
    dlerror();auto result=reinterpret_cast<T>(dlsym(library,name));
    require(result&&!dlerror(),"PSP pinned core API is unavailable");return result;
}
struct Api {
    void* library=nullptr;std::string path;
    decltype(&retro_set_environment) environment;
    decltype(&retro_set_video_refresh) video;
    decltype(&retro_set_audio_sample) audio;
    decltype(&retro_set_audio_sample_batch) audioBatch;
    decltype(&retro_set_input_poll) poll;
    decltype(&retro_set_input_state) input;
    decltype(&retro_api_version) version;
    decltype(&retro_get_system_av_info) av;
    decltype(&retro_init) init;
    decltype(&retro_deinit) deinit;
    decltype(&retro_load_game) load;
    decltype(&retro_unload_game) unload;
    decltype(&retro_run) run;
    decltype(&retro_reset) reset;
    decltype(&retro_serialize_size) stateSize;
    decltype(&retro_serialize) serialize;
    decltype(&retro_unserialize) unserialize;
    void (*registerThreads)(void(*)(),void(*)());
    void (*threadPause)();
    void (*threadStart)();
    const std::atomic<Libretro::EmuThreadState>* threadState=nullptr;
    void (*controlButtons)(uint32_t,uint32_t);
    void (*controlAnalog)(int,float,float);
    void open(const std::string& file){
        if(library){require(path==file,"Cannot replace the loaded PSP core in a live process");return;}
        void* loaded=dlopen(file.c_str(),RTLD_NOW|RTLD_LOCAL);
        require(loaded,"Cannot load packaged PSP core");
        try{
#define API(field,name) field=symbol<decltype(field)>(loaded,#name)
            API(environment,retro_set_environment);API(video,retro_set_video_refresh);
            API(audio,retro_set_audio_sample);API(audioBatch,retro_set_audio_sample_batch);
            API(poll,retro_set_input_poll);API(input,retro_set_input_state);API(version,retro_api_version);
            API(av,retro_get_system_av_info);API(init,retro_init);API(deinit,retro_deinit);
            API(load,retro_load_game);API(unload,retro_unload_game);API(run,retro_run);API(reset,retro_reset);
            API(stateSize,retro_serialize_size);API(serialize,retro_serialize);API(unserialize,retro_unserialize);
#undef API
            registerThreads=symbol<decltype(registerThreads)>(loaded,"_Z20RegisterAttachDetachPFvvES0_");
            threadPause=symbol<decltype(threadPause)>(loaded,"_ZN8Libretro14EmuThreadPauseEv");
            threadStart=symbol<decltype(threadStart)>(loaded,"_ZN8Libretro14EmuThreadStartEv");
            threadState=symbol<decltype(threadState)>(loaded,"_ZN8Libretro14emuThreadStateE");
            controlButtons=symbol<decltype(controlButtons)>(loaded,"_Z19__CtrlUpdateButtonsjj");
            controlAnalog=symbol<decltype(controlAnalog)>(loaded,"_Z17__CtrlSetAnalogXYiff");
            require(version()==RETRO_API_VERSION,"PSP libretro API mismatch");
            library=loaded;path=file;
        }catch(...){dlclose(loaded);throw;}
        // Process-pinned once: never dlclose live C++ TLS code from core threads.
        // Each session still unloads/deinitializes and joins its workers.
    }
    void pauseWorker(){
        using State=Libretro::EmuThreadState;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(threadState->load()==State::START_REQUESTED){
            require(std::chrono::steady_clock::now()<deadline,"PSP worker did not acknowledge resume before pause");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        threadPause();
        require(threadState->load()==State::PAUSED,"PSP worker pause was not acknowledged");
    }
};
Api core;
std::mutex creationMutex;
}

struct Session::Impl {
    static std::atomic<Impl*> active;
    const std::thread::id owner=std::this_thread::get_id();
    Config config;
    EGLDisplay display=EGL_NO_DISPLAY;
    EGLConfig eglConfig{};
    EGLContext context=EGL_NO_CONTEXT;
    EGLSurface parking=EGL_NO_SURFACE,windowSurface=EGL_NO_SURFACE;
    ANativeWindow* nativeWindow=nullptr;
    GLuint framebuffer=0,texture=0,depth=0;
    retro_hw_render_callback hardware{};
    std::map<std::string,std::string> options;
    std::atomic<const char*> callbackFailure{nullptr};
    std::atomic<uint16_t> buttons{0};
    std::atomic<int16_t> stickX{0},stickY{0};
    std::mutex audioMutex;
    std::array<int16_t,kAudioFrames*2> pcm{};
    unsigned queuedAudio=0;
    std::atomic<uint64_t> audioCount{0},nonzeroAudio{0},droppedAudio{0};
    uint64_t frames=0,presented=0,priorErrors=0,contextGeneration=0;
    bool initialized=false,loaded=false,contextReset=false,closed=false,contentReady=false;
    std::atomic<bool> stopped{false};
    bool captureHeld=false,suspended=false;
    double refreshRate=60,aspect=16.0/9.0;
    unsigned rate=44100;
    std::vector<uint8_t> state;

    explicit Impl(Config value):config(std::move(value)){}
    void check()const{require(std::this_thread::get_id()==owner,"PSP operation outside owner thread");require(!closed,"PSP session closed");}
    void healthy(){check();require(!captureHeld,"Release PSP state capture before running core");
        if(auto failure=callbackFailure.load())throw std::runtime_error(failure);require(!stopped,"PSP core requested shutdown");}
    void releaseInput(){
        buttons=0;stickX=0;stickY=0;
        // sceCtrl's current physical input is intentionally not serialized by
        // this pin. Clearing only our callback packet leaves the last held key
        // in the OS while unserialize briefly resumes its worker. Release via
        // the pin's own mutex-protected input API, never by editing guest RAM.
        if(contentReady){core.controlButtons(0,UINT32_MAX);core.controlAnalog(0,0,0);core.controlAnalog(1,0,0);}
    }
    void fail(const char* message){const char* expected=nullptr;callbackFailure.compare_exchange_strong(expected,message);}
    static Impl* get(){return active.load(std::memory_order_acquire);}
    static void log(retro_log_level level,const char* format,...){
        const int priority=level==RETRO_LOG_ERROR?ANDROID_LOG_ERROR:level==RETRO_LOG_WARN?ANDROID_LOG_WARN:ANDROID_LOG_DEBUG;
        va_list arguments;va_start(arguments,format);
        __android_log_vprint(priority,"EmuOrbitPSPCore",format,arguments);va_end(arguments);
    }
    static void defaults(const retro_core_option_v2_definition* values){
        auto* self=get();if(!self||!values)return;
        for(unsigned i=0;i<512&&values[i].key;++i){
            const char* value=values[i].default_value?values[i].default_value:values[i].values[0].value;
            if(value)self->options[values[i].key]=value;
        }
    }
    static bool environment(unsigned command,void* data){
        auto* self=get();if(!self)return false;
        if(command==RETRO_ENVIRONMENT_SHUTDOWN){self->stopped=true;return true;}
        if(command==RETRO_ENVIRONMENT_GET_INPUT_BITMASKS)return true;
        if(!data)return false;
        switch(command){
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:static_cast<retro_log_callback*>(data)->log=log;return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:*static_cast<const char**>(data)=self->config.systemDirectory.c_str();return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:*static_cast<const char**>(data)=self->config.saveDirectory.c_str();return true;
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:*static_cast<unsigned*>(data)=2;return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:defaults(static_cast<retro_core_options_v2*>(data)->definitions);return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:{
            auto* options=static_cast<retro_core_options_v2_intl*>(data);
            if(!options->us)return false;
            defaults(options->us->definitions);return true;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE:{
            auto* variable=static_cast<retro_variable*>(data);if(!variable->key)return false;
            if(!strcmp(variable->key,"ppsspp_backend"))variable->value="opengl";
            else if(!strcmp(variable->key,"ppsspp_cpu_core"))variable->value="JIT";
            else if(!strcmp(variable->key,"ppsspp_internal_resolution"))variable->value="480x272";
            else if(!strcmp(variable->key,"ppsspp_cropto16x9"))variable->value="disabled";
            else if(!strcmp(variable->key,"ppsspp_enable_wlan"))variable->value="disabled";
            else{auto found=self->options.find(variable->key);variable->value=found==self->options.end()?nullptr:found->second.c_str();}
            return variable->value!=nullptr;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:*static_cast<bool*>(data)=false;return true;
        case RETRO_ENVIRONMENT_GET_LANGUAGE:*static_cast<unsigned*>(data)=RETRO_LANGUAGE_ENGLISH;return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:*static_cast<bool*>(data)=true;return true;
        case RETRO_ENVIRONMENT_GET_FASTFORWARDING:*static_cast<bool*>(data)=false;return true;
        case RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE:*static_cast<float*>(data)=60.0f;return true;
        case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:*static_cast<retro_hw_context_type*>(data)=RETRO_HW_CONTEXT_OPENGLES3;return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:return *static_cast<retro_pixel_format*>(data)==RETRO_PIXEL_FORMAT_XRGB8888;
        case RETRO_ENVIRONMENT_SET_HW_RENDER:{
            auto* request=static_cast<retro_hw_render_callback*>(data);
            if(request->context_type!=RETRO_HW_CONTEXT_OPENGLES2&&request->context_type!=RETRO_HW_CONTEXT_OPENGLES3
                    &&request->context_type!=RETRO_HW_CONTEXT_OPENGLES_VERSION)return false;
            request->get_current_framebuffer=currentFramebuffer;request->get_proc_address=procedure;
            self->hardware=*request;return true;
        }
        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:{
            auto* av=static_cast<retro_system_av_info*>(data);
            if(av->geometry.base_width!=kWidth||av->geometry.base_height!=kHeight){self->fail("Unsupported PSP frame geometry");return false;}
            if(!std::isfinite(av->timing.fps)||av->timing.fps<20||av->timing.fps>120||av->timing.sample_rate!=self->rate){
                self->fail("Unsupported PSP timing change");return false;
            }
            self->refreshRate=av->timing.fps;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_GEOMETRY:{
            auto* geometry=static_cast<retro_game_geometry*>(data);
            if(geometry->base_width!=kWidth||geometry->base_height!=kHeight){self->fail("Unsupported PSP geometry change");return false;}
            return true;
        }
        case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:*static_cast<unsigned*>(data)=0;return true;
        case RETRO_ENVIRONMENT_SET_MESSAGE:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
        case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:return true;
        default:return false;
        }
    }
    static uintptr_t currentFramebuffer(){auto* self=get();return self?self->framebuffer:0;}
    static retro_proc_address_t procedure(const char* name){
        auto function=eglGetProcAddress(name);
        return function?reinterpret_cast<retro_proc_address_t>(function):reinterpret_cast<retro_proc_address_t>(dlsym(RTLD_DEFAULT,name));
    }
    static void poll(){}
    static int16_t input(unsigned port,unsigned device,unsigned index,unsigned id){
        auto* self=get();if(!self||port)return 0;
        uint16_t buttons=self->buttons.load();
        if(device==RETRO_DEVICE_JOYPAD)return id==RETRO_DEVICE_ID_JOYPAD_MASK?static_cast<int16_t>(buttons):(id<16?(buttons>>id)&1:0);
        if(device==RETRO_DEVICE_ANALOG&&index==RETRO_DEVICE_INDEX_ANALOG_LEFT)
            return id==RETRO_DEVICE_ID_ANALOG_X?self->stickX.load():id==RETRO_DEVICE_ID_ANALOG_Y?self->stickY.load():0;
        return 0;
    }
    static void audio(int16_t left,int16_t right){int16_t pair[]={left,right};audioBatch(pair,1);}
    static size_t audioBatch(const int16_t* samples,size_t count){
        auto* self=get();if(!self)return count;
        size_t offered=count;
        if((!samples&&count)||count>65536){self->fail("Invalid PSP PCM callback");return 0;}
        std::lock_guard<std::mutex> guard(self->audioMutex);
        self->audioCount+=count;
        for(size_t i=0;i<count*2;++i)if(samples[i])++self->nonzeroAudio;
        if(count>kAudioFrames){self->droppedAudio+=count-kAudioFrames;samples+=(count-kAudioFrames)*2;count=kAudioFrames;}
        if(self->queuedAudio+count>kAudioFrames){unsigned discard=self->queuedAudio+count-kAudioFrames;
            memmove(self->pcm.data(),self->pcm.data()+discard*2,(self->queuedAudio-discard)*4);
            self->queuedAudio-=discard;self->droppedAudio+=discard;}
        if(count)memcpy(self->pcm.data()+self->queuedAudio*2,samples,count*4);
        self->queuedAudio+=count;return offered;
    }
    void priorGlErrors(){
        for(unsigned i=0;i<16;++i){if(glGetError()==GL_NO_ERROR)return;++priorErrors;}
        fail("PSP core GL error queue did not drain");
    }
    static void video(const void* data,unsigned width,unsigned height,size_t){
        auto* self=get();if(!self)return;
        if(std::this_thread::get_id()!=self->owner){self->fail("PSP graphics callback outside owner");return;}
        if(!data){if(self->frames&&self->windowSurface!=EGL_NO_SURFACE){self->priorGlErrors();self->present();}return;}
        // Explicit no-crop option keeps the native PSP geometry and all rows.
        if(data!=RETRO_HW_FRAME_BUFFER_VALID||width!=kWidth||height!=kHeight){
            __android_log_print(ANDROID_LOG_ERROR,"EmuOrbitPSP","Unexpected frame: hardware=%d size=%ux%u before=%llu",
                    data==RETRO_HW_FRAME_BUFFER_VALID,width,height,static_cast<unsigned long long>(self->frames));
            self->fail("Invalid PSP hardware framebuffer");return;
        }
        ++self->frames;self->priorGlErrors();
        if(self->windowSurface!=EGL_NO_SURFACE)self->present();
    }
    void graphics(){
        display=eglGetDisplay(EGL_DEFAULT_DISPLAY);
        require(display!=EGL_NO_DISPLAY&&eglInitialize(display,nullptr,nullptr),"PSP EGL initialization failed");
        require(eglBindAPI(EGL_OPENGL_ES_API),"PSP GLES binding failed");
        const EGLint attributes[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT|EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,
                EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_DEPTH_SIZE,24,EGL_NONE};
        EGLint count=0;require(eglChooseConfig(display,attributes,&eglConfig,1,&count)&&count==1,"PSP EGL configuration unavailable");
        const EGLint contextAttributes[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
        context=eglCreateContext(display,eglConfig,EGL_NO_CONTEXT,contextAttributes);
        require(context!=EGL_NO_CONTEXT,"PSP GLES context failed");
        const EGLint pbuffer[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};
        parking=eglCreatePbufferSurface(display,eglConfig,pbuffer);
        require(parking!=EGL_NO_SURFACE&&eglMakeCurrent(display,parking,parking,context),"PSP parking surface failed");
        ++contextGeneration;
        glGenFramebuffers(1,&framebuffer);glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
        glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
        glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,kWidth,kHeight);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
        glGenRenderbuffers(1,&depth);glBindRenderbuffer(GL_RENDERBUFFER,depth);
        glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,kWidth,kHeight);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_RENDERBUFFER,depth);
        require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE&&glGetError()==GL_NO_ERROR,"PSP framebuffer allocation failed");
        glViewport(0,0,kWidth,kHeight);
    }
    void replaceWindow(ANativeWindow* acquired){
        check();
        if(acquired==nativeWindow){if(acquired)ANativeWindow_release(acquired);return;}
        if(!eglMakeCurrent(display,parking,parking,context)){if(acquired)ANativeWindow_release(acquired);throw std::runtime_error("PSP context cannot park");}
        if(windowSurface!=EGL_NO_SURFACE)eglDestroySurface(display,windowSurface);
        if(nativeWindow)ANativeWindow_release(nativeWindow);
        windowSurface=EGL_NO_SURFACE;nativeWindow=nullptr;
        if(!acquired)return;
        EGLint format=0;eglGetConfigAttrib(display,eglConfig,EGL_NATIVE_VISUAL_ID,&format);
        if(ANativeWindow_setBuffersGeometry(acquired,0,0,format)<0){ANativeWindow_release(acquired);throw std::runtime_error("PSP window geometry failed");}
        EGLSurface created=eglCreateWindowSurface(display,eglConfig,acquired,nullptr);
        if(created==EGL_NO_SURFACE){ANativeWindow_release(acquired);throw std::runtime_error("PSP window surface failed");}
        if(!eglMakeCurrent(display,created,created,context)){
            eglDestroySurface(display,created);ANativeWindow_release(acquired);throw std::runtime_error("PSP window activation failed");}
        windowSurface=created;nativeWindow=acquired;
        eglSwapInterval(display,1);
    }
    void present(){
        EGLint width=0,height=0;
        if(!eglQuerySurface(display,windowSurface,EGL_WIDTH,&width)||!eglQuerySurface(display,windowSurface,EGL_HEIGHT,&height)||width<1||height<1){fail("PSP window size unavailable");return;}
        GLint oldRead=0,oldDraw=0,readBuffer=0;GLfloat clear[4];GLboolean mask[4];
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&oldRead);glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&oldDraw);
        glGetFloatv(GL_COLOR_CLEAR_VALUE,clear);glGetBooleanv(GL_COLOR_WRITEMASK,mask);
        GLboolean scissor=glIsEnabled(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER,framebuffer);glGetIntegerv(GL_READ_BUFFER,&readBuffer);
        if(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){fail("PSP presentation framebuffer incomplete");return;}
        glReadBuffer(GL_COLOR_ATTACHMENT0);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,0);
        glDisable(GL_SCISSOR_TEST);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
        int fittedWidth=width,fittedHeight=static_cast<int>(width/aspect);
        if(fittedHeight>height){fittedHeight=height;fittedWidth=static_cast<int>(height*aspect);}
        int left=(width-fittedWidth)/2,bottom=(height-fittedHeight)/2;
        glBlitFramebuffer(0,hardware.bottom_left_origin?0:kHeight,kWidth,hardware.bottom_left_origin?kHeight:0,
                left,bottom,left+fittedWidth,bottom+fittedHeight,GL_COLOR_BUFFER_BIT,GL_LINEAR);
        glReadBuffer(readBuffer);glBindFramebuffer(GL_READ_FRAMEBUFFER,oldRead);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,oldDraw);
        glClearColor(clear[0],clear[1],clear[2],clear[3]);glColorMask(mask[0],mask[1],mask[2],mask[3]);
        if(scissor)glEnable(GL_SCISSOR_TEST);
        if(glGetError()!=GL_NO_ERROR){fail("PSP presentation GL operation failed");return;}
        if(!eglSwapBuffers(display,windowSurface)){fail("PSP window presentation failed");return;}
        ++presented;
    }
    void start(){
        core.open(config.core);graphics();
        core.registerThreads(config.attachThread,config.detachThread);
        core.environment(environment);core.video(video);core.audio(audio);core.audioBatch(audioBatch);core.poll(poll);core.input(input);
        core.init();initialized=true;
        retro_game_info game{config.media.c_str(),nullptr,0,nullptr};
        require(core.load(&game),"PSP media boot failed");loaded=true;
        require(hardware.context_reset,"PSP hardware context negotiation failed");
        hardware.context_reset();contextReset=true;
        retro_system_av_info info{};core.av(&info);
        require(std::isfinite(info.timing.fps)&&info.timing.fps>=20&&info.timing.fps<=120,"PSP timing invalid");
        require(info.timing.sample_rate>=8000&&info.timing.sample_rate<=192000,"PSP PCM rate invalid");
        refreshRate=info.timing.fps;rate=static_cast<unsigned>(info.timing.sample_rate);
        if(info.geometry.aspect_ratio>1&&info.geometry.aspect_ratio<3)aspect=info.geometry.aspect_ratio;
        healthy();
    }
    void close(){
        if(closed)return;check();
        state.clear();state.shrink_to_fit();captureHeld=false;
        // Keep active callbacks/context until core shutdown has joined workers.
        // The pin's Start is asynchronous while Stop only joins RUNNING. An
        // immediate Start -> unload can skip the join, leaving an old worker
        // running against the next owner's ctx. Observe the real acknowledgement
        // before unload; timeout quarantines the owner, never releases its fence.
        if(loaded){
            using State=Libretro::EmuThreadState;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            auto current=core.threadState->load();
            if(current==State::PAUSED){core.threadStart();current=core.threadState->load();}
            while(current==State::START_REQUESTED||current==State::PAUSE_REQUESTED){
                require(std::chrono::steady_clock::now()<deadline,"PSP worker did not acknowledge shutdown preparation");
                if(current==State::PAUSE_REQUESTED){
                    // A native call cannot normally leave this transient state;
                    // refuse to free callbacks if it does not settle.
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    current=core.threadState->load();
                    if(current==State::PAUSED){core.threadStart();current=core.threadState->load();}
                }else{
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));current=core.threadState->load();
                }
            }
            require(current==State::RUNNING||current==State::DISABLED||current==State::STOPPED,
                    "PSP worker cannot safely unload");
            core.unload();
            current=core.threadState->load();
            require(current==State::STOPPED||current==State::DISABLED,"PSP worker shutdown was not acknowledged");
            loaded=false;
        }
        if(initialized){core.deinit();initialized=false;}
        contextReset=false;active.store(nullptr,std::memory_order_release);
        if(display!=EGL_NO_DISPLAY){
            if(context!=EGL_NO_CONTEXT&&parking!=EGL_NO_SURFACE)eglMakeCurrent(display,parking,parking,context);
            if(framebuffer)glDeleteFramebuffers(1,&framebuffer);
            if(texture)glDeleteTextures(1,&texture);if(depth)glDeleteRenderbuffers(1,&depth);
            eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
            if(windowSurface!=EGL_NO_SURFACE)eglDestroySurface(display,windowSurface);
            if(parking!=EGL_NO_SURFACE)eglDestroySurface(display,parking);
            if(context!=EGL_NO_CONTEXT)eglDestroyContext(display,context);
            eglTerminate(display);
        }
        if(nativeWindow)ANativeWindow_release(nativeWindow);
        nativeWindow=nullptr;display=EGL_NO_DISPLAY;closed=true;
    }
};
std::atomic<Session::Impl*> Session::Impl::active{nullptr};

Session::Session(const Config& config){
    std::lock_guard<std::mutex> guard(creationMutex);
    require(!Impl::active.load(),"A PSP session already owns the core");
    impl_=std::make_unique<Impl>(config);Impl::active.store(impl_.get(),std::memory_order_release);
    try{impl_->start();}catch(...){
        try{impl_->close();}
        catch(...){
            // A core that failed to join must keep its callback target alive.
            // active remains occupied, preventing reuse in this process.
            impl_.release();
            __android_log_write(ANDROID_LOG_ERROR,"EmuOrbitPSP","Failed startup quarantined: restart process before retry");
            throw std::runtime_error("PSP startup/shutdown failed; process restart required");
        }
        throw;
    }
}
Session::~Session() noexcept{
    if(impl_&&!impl_->closed){
        try{impl_->close();}
        catch(...){
            // Never free a callback target while native workers might use it.
            impl_.release();
            __android_log_write(ANDROID_LOG_ERROR,"EmuOrbitPSP","Unjoined PSP session quarantined until process exit");
        }
    }
}
void Session::window(ANativeWindow* acquired){impl_->replaceWindow(acquired);}
void Session::input(uint16_t buttons,int16_t x,int16_t y){impl_->check();impl_->buttons=buttons;impl_->stickX=x;impl_->stickY=y;}
Frame Session::step(int16_t* pcm,size_t capacityFrames){
    impl_->healthy();require(pcm&&capacityFrames>=kAudioFrames,"PSP PCM destination too small");
    if(impl_->suspended&&impl_->contentReady){core.threadStart();impl_->suspended=false;}
    uint64_t before=impl_->presented;core.run();impl_->healthy();
    // The pinned audio upload is reached only after asynchronous boot completes.
    // A placeholder hardware callback alone must not authorize state parsing.
    if(impl_->frames&&impl_->audioCount.load()>0)impl_->contentReady=true;
    std::lock_guard<std::mutex> guard(impl_->audioMutex);
    unsigned count=impl_->queuedAudio;if(count)memcpy(pcm,impl_->pcm.data(),count*4);impl_->queuedAudio=0;
    return{count,impl_->presented!=before};
}
bool Session::pixels(uint8_t* rgba,size_t capacity){
    impl_->healthy();require(rgba&&capacity>=kWidth*kHeight*4,"PSP pixel destination too small");
    if(!impl_->frames)return false;
    impl_->priorGlErrors();
    GLint oldRead=0,readBuffer=0,pack=0,alignment=0,row=0,skipRows=0,skipPixels=0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&oldRead);glBindFramebuffer(GL_READ_FRAMEBUFFER,impl_->framebuffer);
    glGetIntegerv(GL_READ_BUFFER,&readBuffer);glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&pack);
    glGetIntegerv(GL_PACK_ALIGNMENT,&alignment);glGetIntegerv(GL_PACK_ROW_LENGTH,&row);
    glGetIntegerv(GL_PACK_SKIP_ROWS,&skipRows);glGetIntegerv(GL_PACK_SKIP_PIXELS,&skipPixels);
    glBindBuffer(GL_PIXEL_PACK_BUFFER,0);glPixelStorei(GL_PACK_ALIGNMENT,1);glPixelStorei(GL_PACK_ROW_LENGTH,0);
    glPixelStorei(GL_PACK_SKIP_ROWS,0);glPixelStorei(GL_PACK_SKIP_PIXELS,0);glReadBuffer(GL_COLOR_ATTACHMENT0);
    require(glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"PSP readback framebuffer incomplete");
    glReadPixels(0,0,kWidth,kHeight,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
    glReadBuffer(readBuffer);glBindFramebuffer(GL_READ_FRAMEBUFFER,oldRead);glBindBuffer(GL_PIXEL_PACK_BUFFER,pack);
    glPixelStorei(GL_PACK_ALIGNMENT,alignment);glPixelStorei(GL_PACK_ROW_LENGTH,row);glPixelStorei(GL_PACK_SKIP_ROWS,skipRows);glPixelStorei(GL_PACK_SKIP_PIXELS,skipPixels);
    require(glGetError()==GL_NO_ERROR,"PSP readback operation failed");
    if(impl_->hardware.bottom_left_origin){std::array<uint8_t,kWidth*4> rowBytes{};
        for(unsigned y=0;y<kHeight/2;++y){auto* upper=rgba+y*kWidth*4;auto* lower=rgba+(kHeight-1-y)*kWidth*4;
            memcpy(rowBytes.data(),upper,rowBytes.size());memcpy(upper,lower,rowBytes.size());memcpy(lower,rowBytes.data(),rowBytes.size());}}
    return true;
}
const uint8_t* Session::capture(size_t& size){
    impl_->healthy();require(impl_->contentReady,"PSP state requested before native content frame");
    impl_->releaseInput();
    size=core.stateSize();require(size>0&&size<=kMaxStateBytes,"PSP native state exceeds local budget");
    impl_->state.assign(size,0);require(core.serialize(impl_->state.data(),size),"PSP native state capture failed");
    core.pauseWorker();impl_->suspended=true;
    impl_->captureHeld=true;return impl_->state.data();
}
void Session::releaseCapture(){impl_->check();impl_->state.clear();impl_->state.shrink_to_fit();impl_->captureHeld=false;}
void Session::restore(const uint8_t* bytes,size_t size){
    impl_->healthy();require(impl_->contentReady&&bytes&&size>0&&size<=kMaxStateBytes,"PSP state restore bounds/readiness");
    impl_->releaseInput();
    require(core.unserialize(bytes,size),"PSP native state restore refused");
    core.pauseWorker();impl_->suspended=true;
    impl_->releaseInput();
    std::lock_guard<std::mutex> guard(impl_->audioMutex);impl_->queuedAudio=0;
}
void Session::reset(){impl_->healthy();require(impl_->contentReady,"PSP reset before completed boot");
    core.pauseWorker();impl_->suspended=true;
    impl_->releaseInput();core.reset();impl_->healthy();
    std::lock_guard<std::mutex> guard(impl_->audioMutex);impl_->queuedAudio=0;}
void Session::pause(){impl_->healthy();if(impl_->contentReady)core.pauseWorker();impl_->suspended=true;
    impl_->releaseInput();
    std::lock_guard<std::mutex> guard(impl_->audioMutex);impl_->queuedAudio=0;}
Stats Session::stats()const{impl_->check();return{impl_->frames,impl_->presented,impl_->audioCount.load(),impl_->nonzeroAudio.load(),
        impl_->droppedAudio.load(),impl_->priorErrors,impl_->contextGeneration};}
double Session::fps()const{impl_->check();return impl_->refreshRate;}
unsigned Session::sampleRate()const{impl_->check();return impl_->rate;}
bool Session::quiescent(){return Impl::active.load(std::memory_order_acquire)==nullptr;}
void Session::close(){impl_->close();}
}
