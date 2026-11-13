#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "android_util.hpp"

// Exercise the production helper through a JNI function table. Each scenario is
// a fresh process so the real initialization cache is never reset by test code.
namespace {
std::string_view failure;
bool exception_pending = false;
int invalid_calls = 0;
_jclass executable, throwable, class_class;
_jobject first_ctor, second_ctor;
_jobjectArray constructors;
alignas(void*) std::array<std::byte, 128> methods{};
int native_marker = 0;
constexpr size_t method_size = 64;

jclass JNICALL find_class(JNIEnv*, const char* name) {
    if (std::strcmp(name, "java/lang/reflect/Executable") == 0)
        return failure == "missing_executable" ? nullptr : &executable;
    if (std::strcmp(name, "java/lang/Throwable") == 0)
        return failure == "missing_throwable" ? nullptr : &throwable;
    if (std::strcmp(name, "java/lang/Class") == 0)
        return failure == "missing_class" ? nullptr : &class_class;
    ++invalid_calls;
    return nullptr;
}
jfieldID JNICALL get_field(JNIEnv*, jclass cls, const char*, const char*) {
    if (cls != &executable) ++invalid_calls;
    return failure == "missing_field" ? nullptr : reinterpret_cast<jfieldID>(1);
}
jmethodID JNICALL get_method(JNIEnv*, jclass cls, const char*, const char*) {
    if (cls != &class_class) ++invalid_calls;
    return failure == "missing_method" ? nullptr : reinterpret_cast<jmethodID>(1);
}
jobject JNICALL call_object(JNIEnv*, jobject obj, jmethodID method, va_list) {
    if (obj != &throwable || !method) ++invalid_calls;
    if (failure == "array_exception") exception_pending = true;
    return failure == "missing_array" ? nullptr : &constructors;
}
jsize JNICALL array_length(JNIEnv*, jarray array) {
    if (array != &constructors) ++invalid_calls;
    if (failure == "length_exception") exception_pending = true;
    return failure == "short_array" ? 1 : 2;
}
jobject JNICALL array_element(JNIEnv*, jobjectArray array, jsize index) {
    if (array != &constructors || index > 1) ++invalid_calls;
    if (failure == "element_exception") exception_pending = true;
    if (failure == "missing_element") return nullptr;
    return index == 0 ? &first_ctor : &second_ctor;
}
jlong JNICALL get_long(JNIEnv*, jobject obj, jfieldID field) {
    if (!obj || !field) { ++invalid_calls; return 0; }
    if (failure == "pointer_exception") exception_pending = true;
    if (failure == "null_pointer") return 0;
    auto base = reinterpret_cast<uintptr_t>(methods.data());
    if (obj == &first_ctor) return static_cast<jlong>(base);
    if (failure == "reversed_methods") return static_cast<jlong>(base - method_size);
    if (failure == "short_method") return static_cast<jlong>(base + sizeof(void*));
    if (failure == "unaligned_method") return static_cast<jlong>(base + method_size - 1);
    if (failure == "distant_methods") return static_cast<jlong>(base + 4096);
    return static_cast<jlong>(base + method_size);
}
jboolean JNICALL exception_check(JNIEnv*) { return exception_pending; }
void JNICALL exception_clear(JNIEnv*) { exception_pending = false; }
void JNICALL exception_describe(JNIEnv*) {}
void JNICALL delete_ref(JNIEnv*, jobject) {}

bool verify_success(JNIEnv* env) {
    if (!util::art::ArtMethod::Init(env) || exception_pending || invalid_calls) return false;
    auto* method = util::art::ArtMethod::FromReflectedMethod(env, &first_ctor);
    if (!method || method->GetData() != &native_marker) return false;
    // Once initialized successfully, further lookups are unnecessary.
    failure = "missing_executable";
    return util::art::ArtMethod::Init(env);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    JNINativeInterface_ api{};
    api.FindClass = find_class;
    api.GetFieldID = get_field;
    api.GetMethodID = get_method;
    api.CallObjectMethodV = call_object;
    api.GetArrayLength = array_length;
    api.GetObjectArrayElement = array_element;
    api.GetLongField = get_long;
    api.ExceptionCheck = exception_check;
    api.ExceptionClear = exception_clear;
    api.ExceptionDescribe = exception_describe;
    api.DeleteLocalRef = delete_ref;
    JNIEnv env{&api};
    void* marker = &native_marker;
    std::memcpy(methods.data() + method_size - 2 * sizeof(void*), &marker, sizeof(marker));
    failure = argv[1];
    if (failure != "success") {
        if (util::art::ArtMethod::Init(&env) || exception_pending || invalid_calls ||
            util::art::ArtMethod::FromReflectedMethod(&env, &first_ctor) != nullptr) {
            std::fprintf(stderr, "FAIL %s: failed initialization published state or used invalid JNI arguments\n", argv[1]);
            return 1;
        }
    }
    failure = {};
    if (!verify_success(&env)) {
        std::fprintf(stderr, "FAIL %s: successful retry did not recover native data\n", argv[1]);
        return 1;
    }
    std::printf("PASS %s\n", argv[1]);
}
