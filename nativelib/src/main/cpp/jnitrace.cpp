//
// Created by zgy on 2025/10/30.
//

#include "jnitrace.h"
#include "HookUtils.h"
#include "logger.h"

const JNINativeInterface* pJFunc = nullptr;
 JNIEnv * jniEnv = nullptr;
JNITraceMap* _g_jni_trace = nullptr;
static bool debug = false;

void initJni()
{
    LOGE("Find Jni");
    typedef jint (*JNI_GetCreatedJavaVMs_t)(JavaVM**, jsize, jsize*);
    JNI_GetCreatedJavaVMs_t getCreatedVMs = (JNI_GetCreatedJavaVMs_t)findSymbolInLibArt("libart.so","JNI_GetCreatedJavaVMs");
    JavaVM* jvm = nullptr;
    jsize count = 0;
    if((size_t)getCreatedVMs == -1)
    {
        LOGE("无法获取 JNI_GetCreatedJavaVMs 符号地址");
        return;
    }
    jint result = getCreatedVMs(&jvm, 1, &count);

    if (result != JNI_OK || count == 0) {
        LOGE("无法获取 JavaVM 实例");
        return ;
    } else{
        LOGE("get JavaVM success");
    }

    JNIEnv* env = nullptr;
    result = jvm->AttachCurrentThread((JNIEnv **) &env, nullptr);

    if (result != JNI_OK || !env) {
        LOGE("无法获取 JNIEnv 实例");
        return ;
    }
    jint jniVersion = env->GetVersion();
    LOGE("get JNIEnv success:%x",jniVersion);
    setup_jfunc(env);
}

void enable_jni_trace_debug(bool enable)
{
    debug = enable;
}

static bool checkSetup()
{
    if(pJFunc == nullptr)
    {
        LOGE("checkJniCall,pJFunc not init");
        return false;
    }
    if(_logger == nullptr)
    {
        LOGE("checkJniCall,_logger not init");
        return false;
    }
    return true;
}

void trace_FindClass(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(!checkSetup())
    {
        return;
    }
    uint64_t classname = QBDI_GPR_GET(gprState, 1);
    if(debug)
    {
        LOGE("JNI FindClass: %s",(char*)classname);
    }
    appendlog("\n[log] JNI FindClass: ");
    appendlog((char*)classname);
    appendlogendl();
}

void trace_GetStringUTFChars(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(!checkSetup())
    {
        return;
    }
    uint64_t jnienv = QBDI_GPR_GET(gprState,0);
    uint64_t jstr = QBDI_GPR_GET(gprState,1);
    uint64_t jbool = QBDI_GPR_GET(gprState,2);
    const char* str = pJFunc->GetStringUTFChars((JNIEnv*)jnienv,(jstring)jstr,(jboolean*)jbool);
    if(debug)
    {
        LOGE("JNI GetStringUTFChars: %s",str);
    }
    appendlog("\n[log] JNI GetStringUTFChars: ");
    appendlog(str);
    appendlogendl();
}

void trace_GetFieldID(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(!checkSetup())
    {
        return;
    }
    uint64_t name = QBDI_GPR_GET(gprState,2);
    uint64_t sig = QBDI_GPR_GET(gprState,3);
    if(debug)
    {
        LOGE("JNI GetFieldID: name:%s,sig:%s",(char*)name,(char*)sig);
    }
    appendlogendl();
    appendlog("[log] JNI GetFieldID: name: ");
    appendlog((char*)name);
    appendlog(", sig:");
    appendlog((char*)sig);
    appendlogendl();
}

void trace_GetByteArrayElements(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(!checkSetup())
    {
        return;
    }
    uint64_t jni = QBDI_GPR_GET(gprState,0);
    uint64_t arr = QBDI_GPR_GET(gprState,1);
    jint size = pJFunc->GetArrayLength(reinterpret_cast<JNIEnv *>(jni), reinterpret_cast<jarray>(arr));
    char* temp = (char*)malloc(2*size);
    memset(temp,0,2*size);
    pJFunc->GetByteArrayRegion(reinterpret_cast<JNIEnv *>(jni), reinterpret_cast<jbyteArray>(arr), 0, size,
                               reinterpret_cast<jbyte *>(temp));
    if(debug)
    {
        LOGE("JNI GetByteArrayElements:%x,%s",size,temp);
    }
    appendlogendl();
    appendformat("[log] JNI GetByteArrayElements:%x,%s",size,temp);
    appendlogendl();
    free(temp);
}

void trace_GetArrayLength(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(!checkSetup())
    {
        return;
    }
    uint64_t jni = QBDI_GPR_GET(gprState,0);
    uint64_t arr = QBDI_GPR_GET(gprState,1);
    jint size = pJFunc->GetArrayLength(reinterpret_cast<JNIEnv *>(jni), reinterpret_cast<jarray>(arr));
    if(debug)
    {
        LOGE("JNI GetArrayLength:%lx,%x",arr,size);
    }
    appendlogendl();
    appendformat("[log] JNI GetArrayLength:%lx,%x",arr,size);
    appendlogendl();
}

void trace_GetByteArrayRegion(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(!checkSetup())
    {
        return;
    }
    uint64_t x0 = QBDI_GPR_GET(gprState,0);
    uint64_t x1 = QBDI_GPR_GET(gprState,1);
    uint64_t x2 = QBDI_GPR_GET(gprState,2);
    uint64_t len = QBDI_GPR_GET(gprState,3);
    uint64_t jbuf = QBDI_GPR_GET(gprState,4);
    char* tmp = (char*) malloc(len);
    memset(tmp,0,len);
    pJFunc->GetByteArrayRegion(reinterpret_cast<JNIEnv *>(x0), reinterpret_cast<jbyteArray>(x1), x2, len,
                               reinterpret_cast<jbyte *>(tmp));

    if(debug)
    {
        LOGE("JNI GetByteArrayRegion: buf:%lx,%lx,%s",jbuf,len,tmp);
    }
    appendlogendl();
    appendformat("[log] JNI GetByteArrayRegion: %lx,%lx,%s",jbuf,len,tmp);
    appendlogendl();
    free(tmp);
}

void trace_GetIntField(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(!checkSetup())
    {
        return;
    }
    uint64_t jnienv = QBDI_GPR_GET(gprState,0);
    uint64_t jobj = QBDI_GPR_GET(gprState,1);
    uint64_t jfield = QBDI_GPR_GET(gprState,2);
    jint res = pJFunc->GetIntField((JNIEnv*)jnienv,(jobject)jobj,(jfieldID)jfield);
    if(debug)
    {
        LOGE("JNI GetIntField: filedId:%lx,res:%x",jfield,res);
    }
    appendlogendl();
    appendlog("[log] JNI GetIntField: filedId:");
    appendformat("%lx",jfield);
    appendlog(",res:");
    appendformat("%x",res);
    appendlogendl();
}

void trace_GetStaticFieldID(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(!checkSetup())
    {
        return;
    }
    uint64_t name = QBDI_GPR_GET(gprState,2);
    if(debug)
    {
        LOGE("JNI GetStaticFieldID: %s",(char*)name);
    }
    appendlogendl();
    appendlog("[log] JNI GetStaticFieldID: ");
    appendlog((char*)name);
    appendlogendl();
}

void trace_CallStaticObjectMethodV(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(!checkSetup())
    {
        return;
    }
    if(debug)
    {
        LOGE("JNI CallStaticObjectMethodV");
    }
    appendlogendl();
    appendlog("[log] JNI CallStaticObjectMethodV");
    appendlogendl();
}
void trace_GetStaticMethodID(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(!checkSetup())
    {
        return;
    }
    uint64_t name = QBDI_GPR_GET(gprState,2);
    if(debug)
    {
        LOGE("JNI GetStaticMethodID: %s",(char*)name);
    }
    appendlogendl();
    appendlog("[log] JNI GetStaticMethodID: ");
    appendlog((char*)name);
    appendlogendl();
}
void trace_GetLongField(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(!checkSetup())
    {
        return;
    }
    uint64_t jnienv = QBDI_GPR_GET(gprState,0);
    uint64_t jobj = QBDI_GPR_GET(gprState,1);
    uint64_t jfield = QBDI_GPR_GET(gprState,2);
    jlong res = pJFunc->GetLongField((JNIEnv*)jnienv,(jobject)jobj,(jfieldID)jfield);
    if(debug)
    {
        LOGE("JNI GetLongField: filedId:%lx,res:%lx",jfield,res);
    }
    appendlogendl();
    appendlog("[log] JNI GetLongField: filedId:");
    appendformat("%lx",jfield);
    appendlog(",res:");
    appendformat("%lx",res);
    appendlogendl();
}

void trace_RegisterNatives(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(!checkSetup())
    {
        return;
    }
    uint64_t funcs = QBDI_GPR_GET(gprState,2);
    JNINativeMethod* nativeMethods = (JNINativeMethod*)funcs;
    uint64_t func_num = QBDI_GPR_GET(gprState,3);
    appendlogendl();
    for(int i = 0;i<func_num;i++)
    {
        if(debug)
        {
            LOGE("JNI RegisterNatives: %s,%s,%p",nativeMethods[i].signature,nativeMethods[i].name,(void*)((size_t)nativeMethods[i].fnPtr - _g_trace_data->base));
        }
        appendlog("[log] JNI RegisterNatives: ");
        appendlog(nativeMethods[i].signature);
        appendlog(",");
        appendlog(nativeMethods[i].name);
        appendlog(",");
        appendformat("%lx",(size_t)nativeMethods[i].fnPtr - _g_trace_data->base);
        appendlogendl();
    }
}

void trace_GetMethodID(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(!checkSetup())
    {
        return;
    }
    uint64_t str = QBDI_GPR_GET(gprState, 2);
    if(debug)
    {
        LOGE("JNI GetMethodID: %s",(char*)str);
    }
    appendlogendl();
    appendlog("[log] JNI GetMethodID: ");
    appendlog((char*)str);
    appendlogendl();
}

void trace_NewString(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(!checkSetup())
    {
        return;
    }
    uint64_t str = QBDI_GPR_GET(gprState, 1);
    if(debug)
    {
        LOGE("JNI NewString: %s",(char*)str);
    }
    appendlogendl();
    appendlog("[log] JNI NewString: ");
    appendlog((char*)str);
    appendlogendl();
}

void trace_NewStringUTF(QBDI::VM *vm, QBDI::GPRState *gprState)
{
    if(!checkSetup())
    {
        return;
    }
    uint64_t str = QBDI_GPR_GET(gprState, 1);
    if(debug)
    {
        LOGE("JNI NewStringUTF: %s",(char*)str);
    }
    appendlogendl();
    appendlog("[log] JNI NewStringUTF: ");
    appendlog((char*)str);
    appendlogendl();
}

void addJNITrace(void* target,const char* funcname,TraceCallBack callback)
{
    if(_g_jni_trace == nullptr)
    {
        LOGE("jnitrace: not init!");
        return;
    }
    auto it = _g_jni_trace->map.find((size_t)target);
    if(it != _g_jni_trace->map.end())
    {
        LOGE("jnitrace: %p has installed!",target);
        return;
    }
    auto* jni_trace_func = new TraceFunc();
    jni_trace_func->callback = callback;
    _g_jni_trace->map[(size_t)target] = jni_trace_func;
    LOGE("jnitrace: %s,%p install",funcname,target);
}

void setup_jfunc(JNIEnv * jnienv)
{
    jniEnv = jnienv;
    pJFunc = jnienv->functions;
    _g_jni_trace = new JNITraceMap();
    _g_jni_trace->map.reserve(10);
}