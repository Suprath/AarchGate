#include <jni.h>
#include "apex/apex_c_api.h"
#include <vector>
#include <string>
#include <stdlib.h>
#include <string.h>

extern "C" {

JNIEXPORT jlong JNICALL Java_com_aarchgate_engine_AarchGateEngine_nativeCreate(JNIEnv *env, jobject obj) {
    return reinterpret_cast<jlong>(apex_create());
}

JNIEXPORT jlong JNICALL Java_com_aarchgate_engine_AarchGateEngine_nativeCreateUniversalTestLogic(JNIEnv *env, jclass clazz) {
    return reinterpret_cast<jlong>(apex_create_universal_test_logic());
}

JNIEXPORT void JNICALL Java_com_aarchgate_engine_AarchGateEngine_nativeDestroy(JNIEnv *env, jobject obj, jlong handle) {
    apex_destroy(reinterpret_cast<apex_engine_h>(handle));
}

JNIEXPORT jint JNICALL Java_com_aarchgate_engine_AarchGateEngine_nativeRegisterSchema(
    JNIEnv *env, jobject obj, jlong handle, jstring name, jobjectArray fields, jlong stride) {
    
    const char *name_chars = env->GetStringUTFChars(name, nullptr);
    jsize num_fields = env->GetArrayLength(fields);
    
    std::vector<std::string> cpp_names;
    cpp_names.reserve(num_fields);
    std::vector<apex_field_descriptor_t> c_fields;
    c_fields.reserve(num_fields);
    
    jclass field_class = env->FindClass("com/aarchgate/engine/AarchGateEngine$FieldDescriptor");
    jfieldID name_id = env->GetFieldID(field_class, "name", "Ljava/lang/String;");
    jfieldID offset_id = env->GetFieldID(field_class, "offset", "J");
    jfieldID bitwidth_id = env->GetFieldID(field_class, "bitWidth", "J");
    jfieldID datatype_id = env->GetFieldID(field_class, "dataType", "I");

    for (jsize i = 0; i < num_fields; ++i) {
        jobject field_obj = env->GetObjectArrayElement(fields, i);
        jstring field_name = (jstring)env->GetObjectField(field_obj, name_id);
        const char *fname = env->GetStringUTFChars(field_name, nullptr);
        
        cpp_names.push_back(std::string(fname));
        
        apex_field_descriptor_t desc;
        desc.name = cpp_names.back().c_str();
        desc.offset = env->GetLongField(field_obj, offset_id);
        desc.bit_width = env->GetLongField(field_obj, bitwidth_id);
        desc.data_type = env->GetIntField(field_obj, datatype_id);
        
        c_fields.push_back(desc);
        
        env->ReleaseStringUTFChars(field_name, fname);
        env->DeleteLocalRef(field_obj);
    }
    
    int result = apex_register_schema(
        reinterpret_cast<apex_engine_h>(handle),
        name_chars,
        c_fields.data(),
        num_fields,
        stride
    );
    
    env->ReleaseStringUTFChars(name, name_chars);
    return result;
}

JNIEXPORT jint JNICALL Java_com_aarchgate_engine_AarchGateEngine_nativeSetLogic(
    JNIEnv *env, jobject obj, jlong handle, jstring schemaName, jlong irRootPtr, jint mode) {
    
    const char *name_chars = env->GetStringUTFChars(schemaName, nullptr);
    int result = apex_set_logic(
        reinterpret_cast<apex_engine_h>(handle),
        name_chars,
        reinterpret_cast<void*>(irRootPtr),
        mode
    );
    env->ReleaseStringUTFChars(schemaName, name_chars);
    return result;
}

JNIEXPORT jlong JNICALL Java_com_aarchgate_engine_AarchGateEngine_nativeExecute(
    JNIEnv *env, jobject obj, jlong handle, jobject data, jlong count) {
    
    void* buffer = env->GetDirectBufferAddress(data);
    if (!buffer) return -1;
    
    return apex_execute(reinterpret_cast<apex_engine_h>(handle), buffer, count);
}

} // extern "C"
