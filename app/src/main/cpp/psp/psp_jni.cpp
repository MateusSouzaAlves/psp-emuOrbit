// SPDX-License-Identifier: GPL-2.0-or-later
#include "psp_session.h"
#include <android/native_window_jni.h>
#include <jni.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using emuorbit::psp::Session;
JavaVM* vm=nullptr;
std::unique_ptr<Session> session;
std::atomic<long> ownerTid{0};
std::atomic<jlong> token{0};
std::atomic<jlong> nextToken{1};
std::atomic<bool> faulted{false};
thread_local bool attachedHere=false;
void require(bool condition,const char* reason){if(!condition)throw std::runtime_error(reason);}
long tid(){return static_cast<long>(syscall(__NR_gettid));}
void attach(){
    JNIEnv* env=nullptr;
    if(vm&&vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)==JNI_EDETACHED){
        if(vm->AttachCurrentThread(&env,nullptr)==JNI_OK)attachedHere=true;
    }
}
void detach(){if(attachedHere&&vm){vm->DetachCurrentThread();attachedHere=false;}}
void error(JNIEnv* env,const std::exception& failure){
    if(!env->ExceptionCheck())env->ThrowNew(env->FindClass("java/io/IOException"),failure.what());
}
std::string string(JNIEnv* env,jstring input){
    require(input&&env->GetStringUTFLength(input)>0&&env->GetStringUTFLength(input)<=16384,"Invalid PSP native argument");
    const char* value=env->GetStringUTFChars(input,nullptr);require(value,"PSP argument unavailable");
    std::string result(value);env->ReleaseStringUTFChars(input,value);return result;
}
Session& owned(jlong id,bool allowFault=false){
    require(ownerTid.load()==tid(),"PSP native call outside its owner thread");
    require(id>0&&token.load()==id&&session,"Closed or stale PSP native handle");
    require(allowFault||!faulted.load(),"PSP native session requires safe closure");return *session;
}
template<class Action>void operation(JNIEnv* env,jlong id,Action action){
    try{auto& target=owned(id);try{action(target);}catch(...){faulted=true;throw;}}
    catch(const std::exception& failure){error(env,failure);}
}
jlong create(JNIEnv* env,jclass,jstring library,jstring media,jstring system,jstring saves){
    long expected=0;bool claimed=false;
    try{
        require(ownerTid.compare_exchange_strong(expected,tid()),"PSP core already has an owner");claimed=true;
        emuorbit::psp::Config config{string(env,library),string(env,media),string(env,system),string(env,saves),attach,detach};
        const std::string basename="/libemuorbit_psp_core.so";
        require(config.core.size()>basename.size()&&config.core.compare(config.core.size()-basename.size(),basename.size(),basename)==0,
                "Only packaged PSP core may load");
        // Java resolves these paths from app-owned native/files directories;
        // neither an Intent nor a SAF document supplies a library path.
        session=std::make_unique<Session>(config);faulted=false;
        jlong id=nextToken.fetch_add(1);require(id>0,"PSP native handle space exhausted");token=id;return id;
    }catch(const std::exception& failure){
        if(claimed){session.reset();token=0;ownerTid=0;faulted=false;}
        error(env,failure);return 0;
    }
}
void window(JNIEnv* env,jclass,jlong id,jobject surface){
    operation(env,id,[&](Session& target){
        ANativeWindow* acquired=surface?ANativeWindow_fromSurface(env,surface):nullptr;
        require(!surface||acquired,"PSP Android surface is invalid");target.window(acquired);
    });
}
jint step(JNIEnv* env,jclass,jlong id,jint buttons,jshort x,jshort y,jobject output){
    jint result=0;
    operation(env,id,[&](Session& target){
        auto* pcm=static_cast<int16_t*>(env->GetDirectBufferAddress(output));
        require(pcm&&env->GetDirectBufferCapacity(output)>=emuorbit::psp::kAudioFrames*4,"PSP PCM direct buffer required");
        target.input(static_cast<uint16_t>(buttons),x,y);auto frame=target.step(pcm,emuorbit::psp::kAudioFrames);
        result=static_cast<jint>(frame.audioFrames)|(frame.presented?1<<16:0);
    });return result;
}
jboolean pixels(JNIEnv* env,jclass,jlong id,jobject output){
    bool available=false;
    operation(env,id,[&](Session& target){
        auto* rgba=static_cast<uint8_t*>(env->GetDirectBufferAddress(output));jlong capacity=env->GetDirectBufferCapacity(output);
        require(rgba&&capacity>=emuorbit::psp::kWidth*emuorbit::psp::kHeight*4,"PSP RGBA direct buffer required");
        available=target.pixels(rgba,static_cast<size_t>(capacity));
    });return available?JNI_TRUE:JNI_FALSE;
}
jobject capture(JNIEnv* env,jclass,jlong id){
    jobject result=nullptr;
    operation(env,id,[&](Session& target){size_t size=0;auto* bytes=target.capture(size);
        result=env->NewDirectByteBuffer(const_cast<uint8_t*>(bytes),size);
        if(!result){target.releaseCapture();throw std::runtime_error("PSP state buffer unavailable");}
    });return result;
}
void release(JNIEnv* env,jclass,jlong id){
    try{owned(id,true).releaseCapture();}catch(const std::exception& failure){error(env,failure);}
}
void restore(JNIEnv* env,jclass,jlong id,jobject raw){
    operation(env,id,[&](Session& target){
        auto* bytes=raw?static_cast<const uint8_t*>(env->GetDirectBufferAddress(raw)):nullptr;
        jlong count=raw?env->GetDirectBufferCapacity(raw):0;
        require(bytes&&count>0&&static_cast<size_t>(count)<=emuorbit::psp::kMaxStateBytes,"PSP validated direct state required");
        target.restore(bytes,static_cast<size_t>(count));
    });
}
void reset(JNIEnv* env,jclass,jlong id){operation(env,id,[](Session& target){target.reset();});}
void pauseNative(JNIEnv* env,jclass,jlong id){operation(env,id,[](Session& target){target.pause();});}
jlongArray stats(JNIEnv* env,jclass,jlong id){
    try{auto value=owned(id,true).stats();jlong values[]={static_cast<jlong>(value.hardwareFrames),static_cast<jlong>(value.presentedFrames),
        static_cast<jlong>(value.audioFrames),static_cast<jlong>(value.nonzeroAudioSamples),static_cast<jlong>(value.droppedAudioFrames),
        static_cast<jlong>(value.priorCoreGlErrors),static_cast<jlong>(value.contextGeneration)};
        auto result=env->NewLongArray(7);if(result)env->SetLongArrayRegion(result,0,7,values);return result;
    }catch(const std::exception& failure){error(env,failure);return nullptr;}
}
jdoubleArray timing(JNIEnv* env,jclass,jlong id){
    try{auto& target=owned(id);jdouble values[]={target.fps(),static_cast<double>(target.sampleRate())};
        auto result=env->NewDoubleArray(2);if(result)env->SetDoubleArrayRegion(result,0,2,values);return result;
    }catch(const std::exception& failure){error(env,failure);return nullptr;}
}
void closeNative(JNIEnv* env,jclass,jlong id){
    try{owned(id,true).close();session.reset();token=0;faulted=false;ownerTid=0;}
    catch(const std::exception& failure){faulted=true;error(env,failure);}
}
jboolean quiescent(JNIEnv*,jclass){return Session::quiescent()?JNI_TRUE:JNI_FALSE;}
}
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* runtime,void*){
    vm=runtime;JNIEnv* env=nullptr;
    if(vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)!=JNI_OK)return JNI_ERR;
    jclass type=env->FindClass("com/mateussouza/emuorbit/advance/emulator/psp/PspNativeBridge");if(!type)return JNI_ERR;
    const JNINativeMethod methods[]={
        {"create","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)J",reinterpret_cast<void*>(create)},
        {"window","(JLandroid/view/Surface;)V",reinterpret_cast<void*>(window)},
        {"step","(JISSLjava/nio/ByteBuffer;)I",reinterpret_cast<void*>(step)},
        {"pixels","(JLjava/nio/ByteBuffer;)Z",reinterpret_cast<void*>(pixels)},
        {"capture","(J)Ljava/nio/ByteBuffer;",reinterpret_cast<void*>(capture)},
        {"release","(J)V",reinterpret_cast<void*>(release)},
        {"restore","(JLjava/nio/ByteBuffer;)V",reinterpret_cast<void*>(restore)},
        {"reset","(J)V",reinterpret_cast<void*>(reset)},
        {"pause","(J)V",reinterpret_cast<void*>(pauseNative)},
        {"stats","(J)[J",reinterpret_cast<void*>(stats)},
        {"timing","(J)[D",reinterpret_cast<void*>(timing)},
        {"close","(J)V",reinterpret_cast<void*>(closeNative)},
        {"quiescent","()Z",reinterpret_cast<void*>(quiescent)}
    };
    return env->RegisterNatives(type,methods,sizeof(methods)/sizeof(methods[0]))==JNI_OK?JNI_VERSION_1_6:JNI_ERR;
}
