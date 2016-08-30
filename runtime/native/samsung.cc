#include "field_helper.h"
#include "mirror/art_field-inl.h"
#include "mirror/art_method-inl.h"
#include "mirror/class-inl.h"
#include "scoped_fast_native_object_access.h"
#include "ScopedUtfChars.h"

namespace art {

//----------------------------------------------------
// java.lang.Class

static bool equalMethodParameters(mirror::ArtMethod* method, mirror::ObjectArray<mirror::Class>* parameterTypes) {
  const DexFile::TypeList* params = method->GetParameterTypeList();
  if (params == nullptr)
    return (parameterTypes->GetLength() == 0);

  int32_t numParams = params->Size();
  if (numParams != parameterTypes->GetLength())
    return false;

  for (int32_t i = 0; i < numParams; i++) {
    uint16_t type_idx = params->GetTypeItem(i).type_idx_;
    mirror::Class* param_type = method->GetDexCacheResolvedType(type_idx);
     if (param_type == nullptr) {
       param_type = Runtime::Current()->GetClassLinker()->ResolveType(type_idx, method);
       CHECK(param_type != nullptr || Thread::Current()->IsExceptionPending());
     }

     if (param_type != parameterTypes->Get(i))
       return false;
  }

  return true;
}

static mirror::ArtMethod* getDeclaredMethodInternal(mirror::Class* c, const StringPiece& name, mirror::ObjectArray<mirror::Class>* parameterTypes) {
  mirror::ArtMethod* potentialResult = nullptr;

  for (size_t i = 0; i < c->NumVirtualMethods(); i++) {
    mirror::ArtMethod* method = c->GetVirtualMethod(i);

    if (name != method->GetName())
      continue;

    if (!equalMethodParameters(method, parameterTypes))
      continue;

    if (!method->IsMiranda()) {
      if (!method->IsSynthetic())
        return method;

      // Remember as potential result if it's not a miranda method.
      potentialResult = method;
    }
  }

  for (size_t i = 0; i < c->NumDirectMethods(); i++) {
    mirror::ArtMethod* method = c->GetDirectMethod(i);
    if (method->IsConstructor())
      continue;

    if (name != method->GetName())
      continue;

    if (!equalMethodParameters(method, parameterTypes))
      continue;

    if (!method->IsMiranda() && !method->IsSynthetic())
      return method;

    // Direct methods cannot be miranda methods,
    // so this potential result must be synthetic.
    potentialResult = method;
  }

  return potentialResult;
}

static mirror::ArtMethod* getPublicMethodRecursive(mirror::Class* c, const StringPiece& name, mirror::ObjectArray<mirror::Class>* parameterTypes) {
  // search superclasses
  for (mirror::Class* klass = c; klass != nullptr; klass = klass->GetSuperClass()) {
    mirror::ArtMethod* result = getDeclaredMethodInternal(klass, name, parameterTypes);
    if (result != nullptr && result->IsPublic())
      return result;
  }

  // search iftable which has a flattened and uniqued list of interfaces
  int32_t iftable_count = c->GetIfTableCount();
  mirror::IfTable* iftable = c->GetIfTable();
  for (int32_t i = 0; i < iftable_count; i++) {
    mirror::ArtMethod* result = getPublicMethodRecursive(iftable->GetInterface(i), name, parameterTypes);
    if (result != nullptr && result->IsPublic())
      return result;
  }

  return nullptr;
}

static jobject Class_getMethodNative(JNIEnv* env, jobject javaThis, jstring javaName, jobjectArray javaParameterTypes, jboolean recursivePublicMethods) {
  ScopedObjectAccess soa(env);
  if (UNLIKELY(javaName == nullptr)) {
    ThrowNullPointerException(nullptr, "name == null");
    return nullptr;
  }

  mirror::ObjectArray<mirror::Class>* parameterTypes =
      soa.Decode<mirror::ObjectArray<mirror::Class>*>(javaParameterTypes);
  size_t numParameterTypes = parameterTypes->GetLength();
  for (size_t i = 0; i < numParameterTypes; i++) {
    if (parameterTypes->Get(i) == nullptr) {
      Thread* self = Thread::Current();
      ThrowLocation computed_throw_location = self->GetCurrentLocationForThrow();
      self->ThrowNewException(computed_throw_location, "Ljava/lang/NoSuchMethodException;", "parameter type is null");
      return nullptr;
    }
  }

  mirror::Class* const c = soa.Decode<mirror::Class*>(javaThis);
  ScopedUtfChars name(env, javaName);

  mirror::ArtMethod* method = (recursivePublicMethods == JNI_TRUE) ?
        getPublicMethodRecursive(c, name.c_str(), parameterTypes)
      : getDeclaredMethodInternal(c, name.c_str(), parameterTypes);

  if (method == nullptr)
    return nullptr;

  jobject artMethod = soa.AddLocalReference<jobject>(method);
  jobject reflectMethod = env->AllocObject(WellKnownClasses::java_lang_reflect_Method);
  if (env->ExceptionCheck())
    return nullptr;

  env->SetObjectField(reflectMethod, WellKnownClasses::java_lang_reflect_AbstractMethod_artMethod, artMethod);
  return reflectMethod;
}

static mirror::ArtField* getDeclaredFieldInternal(mirror::Class* c, const StringPiece& name) {
  for (size_t i = 0; i < c->NumInstanceFields(); ++i) {
    mirror::ArtField* f = c->GetInstanceField(i);
    if (name == f->GetName()) {
      return f;
    }
  }

  for (size_t i = 0; i < c->NumStaticFields(); ++i) {
    mirror::ArtField* f = c->GetStaticField(i);
    if (name == f->GetName()) {
      return f;
    }
  }

  return nullptr;
}

static mirror::ArtField* getPublicFieldRecursive(mirror::Class* c, const StringPiece& name) {
  // search superclasses
  for (mirror::Class* klass = c; klass != nullptr; klass = klass->GetSuperClass()) {
    mirror::ArtField* result = getDeclaredFieldInternal(klass, name);
    if (result != nullptr && result->IsPublic())
      return result;
  }

  // search iftable which has a flattened and uniqued list of interfaces
  int32_t iftable_count = c->GetIfTableCount();
  mirror::IfTable* iftable = c->GetIfTable();
  for (int32_t i = 0; i < iftable_count; i++) {
    mirror::ArtField* result = getPublicFieldRecursive(iftable->GetInterface(i), name);
    if (result != nullptr && result->IsPublic())
      return result;
  }

  return nullptr;
}

static jobject getDeclaredOrRecursiveField(JNIEnv* env, jobject javaThis, jstring javaName, bool recursiveFieldMethods) {
  ScopedObjectAccess soa(env);
  ScopedUtfChars name(env, javaName);
  mirror::Class* const c = soa.Decode<mirror::Class*>(javaThis);

  mirror::ArtField* field = recursiveFieldMethods ?
        getPublicFieldRecursive(c, name.c_str())
      : getDeclaredFieldInternal(c, name.c_str());

  if (field == nullptr)
    return nullptr;

  jobject artField = soa.AddLocalReference<jobject>(field);
  jobject reflectField = env->AllocObject(WellKnownClasses::java_lang_reflect_Field);
  if (env->ExceptionCheck())
    return nullptr;

  env->SetObjectField(reflectField, WellKnownClasses::java_lang_reflect_Field_artField, artField);
  return reflectField;
}

static jobject Class_getFieldNative(JNIEnv* env, jobject javaThis, jstring javaName) {
  return getDeclaredOrRecursiveField(env, javaThis, javaName, true);
}

static jobject Class_getDeclaredFieldInternalNative(JNIEnv* env, jobject javaThis, jstring javaName) {
  return getDeclaredOrRecursiveField(env, javaThis, javaName, false);
}


//----------------------------------------------------
// java.lang.reflect.ArtField

static jobject ArtField_getNameNative(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::ArtField* const f = soa.Decode<mirror::Object*>(javaThis)->AsArtField();
  return env->NewStringUTF(f->GetName());
}

static jclass ArtField_getTypeNative(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ArtField> f(hs.NewHandle(soa.Decode<mirror::Object*>(javaThis)->AsArtField()));
  return soa.AddLocalReference<jclass>(FieldHelper(f).GetType());
}


//----------------------------------------------------
// java.lang.reflect.ArtMethod

static jobject ArtMethod_getNameNative(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::ArtMethod* const f = soa.Decode<mirror::Object*>(javaThis)->AsArtMethod();
  return env->NewStringUTF(f->GetName());
}

//----------------------------------------------------
// dalvik.system.PathClassLoader

static jobject PathClassLoader_openNative(JNIEnv* env, jobject javaThis) {
  // Ignore Samsung native method and use the default PathClassLoader constructor
  return nullptr;
}

//----------------------------------------------------
// java.lang.Runtime

static void Runtime_appStartupBegin(JNIEnv* env, jobject javaThis) {
  // Do nothing
}

static void Runtime_appStartupEnd(JNIEnv* env, jobject javaThis) {
  // Do nothing
}

//----------------------------------------------------
static JNINativeMethod gMethodsClass[] = {
  NATIVE_METHOD(Class, getMethodNative, "(Ljava/lang/String;[Ljava/lang/Class;Z)Ljava/lang/reflect/Method;"),
  NATIVE_METHOD(Class, getFieldNative, "(Ljava/lang/String;)Ljava/lang/reflect/Field;"),
  NATIVE_METHOD(Class, getDeclaredFieldInternalNative, "(Ljava/lang/String;)Ljava/lang/reflect/Field;"),
};

static JNINativeMethod gMethodsArtField[] = {
  NATIVE_METHOD(ArtField, getNameNative, "!()Ljava/lang/String;"),
  NATIVE_METHOD(ArtField, getTypeNative, "!()Ljava/lang/Class;"),
};

static JNINativeMethod gMethodsArtMethod[] = {
  NATIVE_METHOD(ArtMethod, getNameNative, "!()Ljava/lang/String;"),
};

static JNINativeMethod gMethodsPathClassLoader[] = {
  NATIVE_METHOD(PathClassLoader, openNative, "!(Ljava/lang/String;Ljava/lang/String;Ljava/lang/ClassLoader;)Ldalvik/system/PathClassLoader;"),
};

static JNINativeMethod gMethodsRuntime[] = {
  NATIVE_METHOD(Runtime, appStartupBegin, "!()V"),
  NATIVE_METHOD(Runtime, appStartupEnd, "!()V"),
};

//----------------------------------------------------
void register_samsung_native_methods(JNIEnv* env) {
  if (!IsSamsungROM())
    return;

  RegisterNativeMethods(env, "java/lang/Class", gMethodsClass, arraysize(gMethodsClass));
  RegisterNativeMethods(env, "java/lang/reflect/ArtField", gMethodsArtField, arraysize(gMethodsArtField));
  RegisterNativeMethods(env, "java/lang/reflect/ArtMethod", gMethodsArtMethod, arraysize(gMethodsArtMethod));
  RegisterNativeMethods(env, "dalvik/system/PathClassLoader", gMethodsPathClassLoader, arraysize(gMethodsPathClassLoader));
  RegisterNativeMethods(env, "java/lang/Runtime", gMethodsRuntime, arraysize(gMethodsRuntime));
}

}  // namespace art
