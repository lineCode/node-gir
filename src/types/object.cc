#include <iostream>
#include <string>

#include "object.h"
#include "function_type.h"
#include "../signal_closure.h"
#include "../util.h"
#include "../function.h"
#include "../values.h"
#include "../namespace_loader.h"
#include "../util.h"

#include <string.h>
#include <node.h>

using namespace v8;
using namespace std;

namespace gir {

void empty_func(void) {};

std::vector<ObjectFunctionTemplate> GIRObject::templates;
std::vector<InstanceData> GIRObject::instances;
GIPropertyInfo *g_object_info_find_property(GIObjectInfo *info, char *name);

GIRObject::GIRObject(GIObjectInfo *info_, int n_params, GParameter *parameters)
{
    info = info_;
    GType t = g_registered_type_info_get_g_type(info);

    abstract = g_object_info_get_abstract(info);
    if (abstract) {
        obj = nullptr;
    }
    else {
        // gobject va_list, to allow construction parameters
        obj = G_OBJECT(g_object_newv(t, n_params, parameters));
        debug_printf("New GObject [%p] '%s' \n", obj,G_OBJECT_TYPE_NAME(obj));
    }
}

GIObjectInfo *
_get_object_info(GType obj_type, GIObjectInfo *info)
{
    // The main purpose of this function is to get the best matching info.
    // For example, we have class C which extends B and this extends A.
    // Function x() returns C instances, while it's introspected to return A instances.
    // If C info is not found, we try B as it's the first ancestor of C, etc.
    GIBaseInfo *tmp_info = g_irepository_find_by_gtype(NamespaceLoader::repo, obj_type);
    if (tmp_info != nullptr && g_base_info_equal(tmp_info, info))
        return g_base_info_ref(tmp_info);
    GType parent_type = g_type_parent(obj_type);
    if (tmp_info == nullptr)
        return g_irepository_find_by_gtype(NamespaceLoader::repo, parent_type);
    return tmp_info;
}

Handle<Value> GIRObject::New(GObject *obj_, GIObjectInfo *info_)
{
    // find the function template
    if (obj_ == nullptr || !G_IS_OBJECT(obj_)) {
        return Nan::Null();
    }

    Handle<Value> res = GetInstance(obj_);
    if (res != Nan::Null()) {
        return res;
    }
    Handle<Value> arg = Nan::New<Boolean>(false);
    std::vector<ObjectFunctionTemplate>::iterator it;

    GIObjectInfo *object_info = _get_object_info(G_OBJECT_TYPE(obj_), info_);
    if (!object_info) {
        gchar *msg = g_strdup_printf("ObjectInfo not found for '%s'", G_OBJECT_TYPE_NAME(obj_));
        Nan::ThrowTypeError(msg);
    }
    for (it = templates.begin(); it != templates.end(); ++it) {
        if (g_base_info_equal(object_info, it->info)) {
            res = it->function->GetFunction()->NewInstance(1, &arg);
            if (!res.IsEmpty()) {
                GIRObject *e = ObjectWrap::Unwrap<GIRObject>(res->ToObject());
                e->info = object_info;
                e->obj = obj_;
                e->abstract = false;
                g_base_info_unref(object_info);
                return res;
            }
            break;
        }
    }
    if (object_info)
	    g_base_info_unref(object_info);

    return Nan::Null();
}

Handle<Value> GIRObject::New(GObject *obj_, GType t)
{
    if (obj_ == nullptr || !G_IS_OBJECT(obj_)) {
        return Nan::Null();
    }

    Handle<Value> res = GetInstance(obj_);
    if (res != Nan::Null()) {
        return res;
    }

    Handle<Value> arg = Nan::New<Boolean>(false);
    std::vector<ObjectFunctionTemplate>::iterator it;
    GIBaseInfo *base_info = g_irepository_find_by_gtype(NamespaceLoader::repo, t);
    if (base_info == nullptr) {
        base_info = g_irepository_find_by_gtype(NamespaceLoader::repo, g_type_parent(t));
    }
    /*printf("CREATE NEW OBJECT WITH TYPE '%s' BASE '%s' \n",
            G_OBJECT_TYPE_NAME(t),
            g_base_info_get_name(base_info)); */
    for (it = templates.begin(); it != templates.end(); ++it) {
        if (t == it->type) {
            res = it->function->GetFunction()->NewInstance(1, &arg);
            if (!res.IsEmpty()) {
                GIRObject *e = ObjectWrap::Unwrap<GIRObject>(res->ToObject());
                e->info = it->info;
                e->obj = obj_;
                e->abstract = false;
                return res;
            }
            return Nan::Null();
        }
    }
    return Nan::Null();
}

NAN_METHOD(GIRObject::New)
{
    if (info.Length() == 1 && info[0]->IsBoolean() && !info[0]->IsTrue()) {
        GIRObject *obj = new GIRObject();
        obj->Wrap(info.This());
        PushInstance(obj, info.This());
        info.GetReturnValue().Set(info.This());
    }

    String::Utf8Value className(info.Callee()->GetName());

    debug_printf ("CTR '%s' \n", *className);
    std::vector<ObjectFunctionTemplate>::iterator it;

    GIObjectInfo *objinfo = nullptr;
    for (it = templates.begin(); it != templates.end(); ++it) {
        if (strcmp(it->type_name, *className) == 0) {
            objinfo = it->info;
            break;
        }
    }

    if (objinfo == nullptr) {
        Nan::ThrowError("no such class. Callee()->GetName() returned wrong classname");
    }

    int length = 0;
    GParameter *params = nullptr;
    Local<Value> v = ToParams(info[0], &params, &length, objinfo);
    if (v != Nan::Null())
        info.GetReturnValue().Set(v);

    GIRObject *obj = new GIRObject(objinfo, length, params);
    DeleteParams(params, length);

    obj->Wrap(info.This());
    PushInstance(obj, info.This());
    info.GetReturnValue().Set(info.This());
}

GIRObject::~GIRObject()
{
    // This destructor willbe called only (and only) if object is garabage collected
    // For persistant destructor see Node::AtExit
    // http://prox.moraphi.com/index.php/https/github.com/bnoordhuis/node/commit/1c20cac
}

Handle<Value> GIRObject::ToParams(Handle<Value> val, GParameter** params, int *length, GIObjectInfo *info)
{
    *length = 0;
    *params = nullptr;
    if (!val->IsObject()) {
        return Nan::Null();
    }
    Handle<Object> obj = val->ToObject();

    Handle<Array> props = obj->GetPropertyNames();
    *length = props->Length();
    *params = g_new0(GParameter, *length);
    GParamSpec *pspec = nullptr;
    GObjectClass *klass = (GObjectClass*) g_type_class_ref(g_type_from_name(g_object_info_get_type_name(info)));
    for (int i=0; i<*length; i++) {
        String::Utf8Value key(props->Get(i)->ToString());

        // nullify name so it can be freed safely
        (*params)[i].name = nullptr;

        if (!FindProperty(info, *key)) {
            /* Try to find property spec registered for given class */
            if (klass) {
                pspec = g_object_class_find_property(klass, *key);
            }

            if (!pspec) {
                DeleteParams(*params, (*length)-1);
                gchar *msg = g_strdup_printf("Can not find '%s' property", *key);
                Nan::ThrowTypeError(msg);
            }
        }

        GValue gvalue = {0, {{0}}};
        GType value_type = G_TYPE_INVALID;
        // Determine the best match for property's type
        if (klass) {
            if (!pspec)
                pspec = g_object_class_find_property(klass, *key);

            if (pspec)
                value_type = pspec->value_type;
        }
        if (!GIRValue::ToGValue(obj->Get(props->Get(i)), value_type, &gvalue)) {
            DeleteParams(*params, (*length)-1);
            gchar *msg = g_strdup_printf("'%s' property value conversion failed", *key);
            Nan::ThrowTypeError(msg);
        }

        (*params)[i].name = g_strdup(*key);
        (*params)[i].value = gvalue;

        pspec = nullptr;
    }

    if (klass)
        g_type_class_unref(klass);

    return Nan::Null();
}

void GIRObject::DeleteParams(GParameter *params, int l)
{
    if (params == nullptr)
        return;

    for (int i=0; i<l; i++) {
        if (params[i].name == nullptr)
            break;
        g_free((gchar *)params[i].name);
        g_value_unset(&params[i].value);
    }
    g_free(params);
}

NAN_PROPERTY_GETTER(PropertyGetHandler)
{
    String::Utf8Value _name(property);
    Handle<External> info_ptr = Handle<External>::Cast(info.Data());
    GIBaseInfo *base_info  = (GIBaseInfo*) info_ptr->Value();
    if (base_info != nullptr) {
        GIRObject *that = Nan::ObjectWrap::Unwrap<GIRObject>(info.This()->ToObject());
        GParamSpec *pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(that->obj), *_name);
        if (pspec) {
            // Property is not readable
            if (!(pspec->flags & G_PARAM_READABLE)) {
                Nan::ThrowTypeError("property is not readable");
            }

            debug_printf("GetHandler (Get property) [%p] '%s.%s' \n", that->obj, G_OBJECT_TYPE_NAME(that->obj), *_name);

            GIPropertyInfo *prop_info = g_object_info_find_property(base_info, *_name);
            GType value_type = G_TYPE_FUNDAMENTAL(pspec->value_type);
            GValue gvalue = {0, {{0}}};
            g_value_init(&gvalue, pspec->value_type);
            g_object_get_property(G_OBJECT(that->obj), *_name, &gvalue);
            Local<Value> res = GIRValue::FromGValue(&gvalue, prop_info);
            if (value_type != G_TYPE_OBJECT && value_type != G_TYPE_BOXED) {
                g_value_unset(&gvalue);
            }
            if (prop_info)
                g_base_info_unref(prop_info);

            info.GetReturnValue().Set(res);
        }
    }

    // Fallback to defaults
    info.GetReturnValue().Set(info.This()->GetPrototype()->ToObject()->Get(property));
}

NAN_PROPERTY_QUERY(PropertyQueryHandler)
{
    String::Utf8Value _name(property);
    debug_printf("QUERY HANDLER '%s' \n", *_name);
    info.GetReturnValue().Set(Nan::New(0));
}

NAN_PROPERTY_SETTER(PropertySetHandler)
{
    String::Utf8Value _name(property);

    v8::Handle<v8::External> info_ptr = v8::Handle<v8::External>::Cast(info.Data());
    GIBaseInfo *base_info  = (GIBaseInfo*) info_ptr->Value();
    if (base_info != nullptr) {
        GIRObject *that = Nan::ObjectWrap::Unwrap<GIRObject>(info.This()->ToObject());
        GParamSpec *pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(that->obj), *_name);
        if (pspec) {
            // Property is not readable
            if (!(pspec->flags & G_PARAM_WRITABLE)) {
                Nan::ThrowTypeError("property is not writable");
            }

            debug_printf("SetHandler (Set property) '%s.%s' \n", G_OBJECT_TYPE_NAME(that->obj), *_name);

            GValue gvalue = {0, {{0}}};
            bool value_is_set = GIRValue::ToGValue(value, pspec->value_type, &gvalue);
            g_object_set_property(G_OBJECT(that->obj), *_name, &gvalue);
            GType value_type = G_TYPE_FUNDAMENTAL(pspec->value_type);
            if (value_type != G_TYPE_OBJECT && value_type != G_TYPE_BOXED) {
                g_value_unset(&gvalue);
            }
            info.GetReturnValue().Set(Nan::New(value_is_set));
        }
    }

    // Fallback to defaults
    info.GetReturnValue().Set(Nan::New<v8::Boolean>(info.This()->GetPrototype()->ToObject()->Set(property, value)));
}

void GIRObject::Prepare(Handle<Object> target, GIObjectInfo *info)
{
    char *name = (char*)g_base_info_get_name(info);
    const char *namespace_ = g_base_info_get_namespace(info);
    g_base_info_ref(info);

    Local<FunctionTemplate> t = Nan::New<FunctionTemplate>(New);
    t->SetClassName(Nan::New<String>(name).ToLocalChecked());

    ObjectFunctionTemplate oft;
    oft.type_name = name;
    oft.info = info;
    oft.function = t;
    oft.type = g_registered_type_info_get_g_type(info);
    oft.namespace_ = (char*)namespace_;

    templates.push_back(oft);

    // Create instance template
    v8::Local<v8::ObjectTemplate> instance_t = t->InstanceTemplate();
    instance_t->SetInternalFieldCount(1);
    // Create external to hold GIBaseInfo and set it
    v8::Handle<v8::External> info_handle = Nan::New<v8::External>((void*)g_base_info_ref(info));
    // Set properties handlers
    SetNamedPropertyHandler(instance_t,
        PropertyGetHandler,
        PropertySetHandler,
        PropertyQueryHandler,
        nullptr,
        nullptr,
        info_handle);

    t->Set(Nan::New<v8::String>("__properties__").ToLocalChecked(), PropertyList(info));
    t->Set(Nan::New<v8::String>("__methods__").ToLocalChecked(), MethodList(info));
    t->Set(Nan::New<v8::String>("__interfaces__").ToLocalChecked(), InterfaceList(info));
    t->Set(Nan::New<v8::String>("__fields__").ToLocalChecked(), FieldList(info));
    t->Set(Nan::New<v8::String>("__signals__").ToLocalChecked(), SignalList(info));
    t->Set(Nan::New<v8::String>("__v_funcs__").ToLocalChecked(), VFuncList(info));
    t->Set(Nan::New<v8::String>("__abstract__").ToLocalChecked(), Nan::New<Boolean>(g_object_info_get_abstract(info)));

    int l = g_object_info_get_n_constants(info);
    for (int i=0; i<l; i++) {
        GIConstantInfo *constant = g_object_info_get_constant(info, i);
        t->Set(Nan::New<String>(g_base_info_get_name(constant)).ToLocalChecked(), Nan::New<Number>(i));
        g_base_info_unref(constant);
    }

    RegisterMethods(target, info, namespace_, t);
    SetPrototypeMethods(t, name);
}

void GIRObject::Initialize(Handle<Object> target, char *namespace_)
{
    // this gets called when all classes have been initialized
    std::vector<ObjectFunctionTemplate>::iterator it;
    std::vector<ObjectFunctionTemplate>::iterator temp;
    GIObjectInfo* parent;

    for (it = templates.begin(); it != templates.end(); ++it) {
        parent = g_object_info_get_parent(it->info);
        if (strcmp(it->namespace_, namespace_) != 0 || !parent) {
            continue;
        }

        for (temp = templates.begin(); temp != templates.end(); ++temp) {
            if (g_base_info_equal(temp->info, parent)) {
                it->function->Inherit(temp->function);
            }
        }
    }
    for (it = templates.begin(); it != templates.end(); ++it) {
        if (strcmp(it->namespace_, namespace_) == 0) {
            target->Set(Nan::New<String>(g_base_info_get_name(it->info)).ToLocalChecked(), it->function->GetFunction());
        }
    }
}

void GIRObject::SetPrototypeMethods(Local<FunctionTemplate> t, char *name)
{
    Nan::SetPrototypeMethod(t, "__get_property__", GetProperty);
    Nan::SetPrototypeMethod(t, "__set_property__", SetProperty);
    Nan::SetPrototypeMethod(t, "__get_interface__", GetInterface);
    Nan::SetPrototypeMethod(t, "__get_field__", GetField);
    Nan::SetPrototypeMethod(t, "__call_v_func__", CallMethod);

    // Add our 'connect' method to the target.
    // This method is used to connect signals to the underlying gobject.
    Nan::SetPrototypeMethod(t, "connect", Connect);
}

void GIRObject::PushInstance(GIRObject *obj, Handle<Value> value)
{
    Local<Object> p_value = value->ToObject();
    obj->MakeWeak();

    InstanceData data;
    data.obj = obj;
    data.instance = p_value;
    instances.push_back(data);
}

Handle<Value> GIRObject::GetInstance(GObject *obj)
{
    std::vector<InstanceData>::iterator it;
    for (it = instances.begin(); it != instances.end(); it++) {
        if (it->obj && it->obj->obj && it->obj->obj == obj) {
            return it->instance;
        }
    }
    return Nan::Null();
}

NAN_METHOD(GIRObject::CallUnknownMethod)
{
    String::Utf8Value fname(info.Callee()->GetName());
    debug_printf("Call method '%s' \n", *fname);
    GIRObject *that = Nan::ObjectWrap::Unwrap<GIRObject>(info.This()->ToObject());
    GIFunctionInfo *func = that->FindMethod(that->info, *fname);
    debug_printf("Call Method: '%s' [%p] \n", *fname, func);

    if (func) {
        debug_printf("\t Call symbol: '%s' \n", g_function_info_get_symbol(func));
        info.GetReturnValue().Set(Func::Call(that->obj, func, info, TRUE));
    }
    else {
        Nan::ThrowError("no such method");
    }
}

NAN_METHOD(GIRObject::CallMethod)
{
    if (info.Length() < 1 || !info[0]->IsString()) {
        Nan::ThrowError("Invalid argument's number or type");
    }

    String::Utf8Value fname(info[0]);
    GIRObject *that = Nan::ObjectWrap::Unwrap<GIRObject>(info.This()->ToObject());
    GIFunctionInfo *func = that->FindMethod(that->info, *fname);

    if (func) {
        info.GetReturnValue().Set(Func::Call(that->obj, func, info, FALSE));
    }
    else {
        Nan::ThrowError("no such method");
    }

    info.GetReturnValue().SetUndefined();
}

/**
 * This method will connect a signal to the underlying gobject
 * using a GirSignalClosure (a custom GClosure we've written to support
 * JS callback's)
 */
NAN_METHOD(GIRObject::Connect) {
    if (info.Length() != 2 || !info[0]->IsString() || !info[1]->IsFunction()) {
        Nan::ThrowError("Invalid arguments: expected (string, Function)");
    }
    GIRObject *self = Nan::ObjectWrap::Unwrap<GIRObject>(info.This()->ToObject());
    Nan::Utf8String nan_signal_name(info[0]->ToString());
    char *signal_name = *nan_signal_name;
    Nan::Persistent<Function, CopyablePersistentTraits<Function>> callback(Nan::To<Function>(info[1]).ToLocalChecked());

    // parse the signal id and detail (whatever that is) from the gobject we're wrapping
    guint signal_id;
    GQuark detail;
    if (!g_signal_parse_name(signal_name, G_OBJECT_TYPE(self->obj), &signal_id, &detail, TRUE)) {
        Nan::ThrowError("unknown signal name");
    }

    // query for in-depth information about the target signal
    // we can pass this to our closure constructor later so that it can
    // find the GI type info for the signal.
    GSignalQuery signal_query;
    g_signal_query(signal_id, &signal_query);

    // create a closure that will manage the signal callback to JS callback for us
    GClosure *closure = gir_new_signal_closure(self, signal_query.itype, signal_name, callback);
    if (closure == NULL) {
        callback.Reset();
        Nan::ThrowError("unknown signal");
    }

    // connect the closure to the signal using the signal_id and detail we've already found
    gulong handle_id = g_signal_connect_closure_by_id(self->obj, signal_id, detail, closure, FALSE); // TODO: support connecting with after=TRUE

    // return the signal connection ID back to JS.
    info.GetReturnValue().Set(Nan::New((uint32_t)handle_id));
}

NAN_METHOD(GIRObject::GetProperty)
{
    if (info.Length() < 1 || !info[0]->IsString()) {
        Nan::ThrowError("Invalid argument's number or type");
    }

    String::Utf8Value propname(info[0]);
    GIRObject *that = Nan::ObjectWrap::Unwrap<GIRObject>(info.This()->ToObject());
    GIPropertyInfo *prop = that->FindProperty(that->info, *propname);

    if (!prop) {
        Nan::ThrowError("no such property");
    }
    if (!(g_property_info_get_flags(prop) & G_PARAM_READABLE)) {
        Nan::ThrowError("property is not readable");
    }

    GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(that->obj), *propname);

    GValue gvalue = {0, {{0}}};
    g_value_init(&gvalue, spec->value_type);
    g_object_get_property(G_OBJECT(that->obj), *propname, &gvalue);

    Local<Value> res = GIRValue::FromGValue(&gvalue, nullptr);
    g_value_unset(&gvalue);

    info.GetReturnValue().Set(res);
}

NAN_METHOD(GIRObject::SetProperty)
{
    if(info.Length() < 2 || !info[0]->IsString()) {
        Nan::ThrowError("Invalid argument's number or type");
    }

    String::Utf8Value propname(info[0]);
    GIRObject *that = Nan::ObjectWrap::Unwrap<GIRObject>(info.This()->ToObject());
    GIPropertyInfo *prop = that->FindProperty(that->info, *propname);

    if (!prop) {
        Nan::ThrowError("no such property");
    }
    if (!(g_property_info_get_flags(prop) & G_PARAM_WRITABLE)) {
        Nan::ThrowError("property is not writable");
    }

    GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(that->obj), *propname);

    GValue gvalue = {0, {{0}}};
    if (!GIRValue::ToGValue(info[1], spec->value_type, &gvalue)) {
        Nan::ThrowError("Cant convert to JS value to c value");
    }
    g_object_set_property(G_OBJECT(that->obj), *propname, &gvalue);

    info.GetReturnValue().SetUndefined();
}

NAN_METHOD(GIRObject::GetInterface)
{
    if (info.Length() < 1 || !info[0]->IsString()) {
        Nan::ThrowError("Invalid argument's number or type");
    }

    String::Utf8Value iname(info[0]);
    GIRObject *that = Nan::ObjectWrap::Unwrap<GIRObject>(info.This()->ToObject());
    GIInterfaceInfo *interface = that->FindInterface(that->info, *iname);

    if (interface) {
        debug_printf("interface %s exists\n", *iname);
    }
    else {
        debug_printf("interface %s does NOT exist\n", *iname);
    }

    info.GetReturnValue().SetUndefined();
}

NAN_METHOD(GIRObject::GetField)
{
    if (info.Length() < 1 || !info[0]->IsString()) {
        Nan::ThrowError("Invalid argument's numer or type");
    }

    String::Utf8Value fname(info[0]);
    GIRObject *that = Nan::ObjectWrap::Unwrap<GIRObject>(info.This()->ToObject());
    GIFieldInfo *field = that->FindField(that->info, *fname);

    if (field) {
        debug_printf("field %s exists\n", *fname);
    }
    else {
        debug_printf("field %s does NOT exist\n", *fname);
    }

    info.GetReturnValue().SetUndefined();
}

NAN_METHOD(GIRObject::CallVFunc)
{
    if (info.Length() < 1 || !info[0]->IsString()) {
        Nan::ThrowError("Invalid argument's number or type");
    }

    String::Utf8Value fname(info[0]);
    GIRObject *that = Nan::ObjectWrap::Unwrap<GIRObject>(info.This()->ToObject());
    GISignalInfo *vfunc = that->FindSignal(that->info, *fname);

    if (vfunc) {
        debug_printf("VFunc %s exists\n", *fname);
    }
    else {
        debug_printf("VFunc %s does NOT exist\n", *fname);
    }

    info.GetReturnValue().SetUndefined();
}

GIFunctionInfo *GIRObject::FindMethod(GIObjectInfo *info, char *js_name)
{
    // given the JS function/method names are camelCased, we should
    // convert to snake case first!
    string snake_case_name = Util::toSnakeCase(string(js_name));
    const char* native_name = snake_case_name.c_str();
    GIFunctionInfo *func = g_object_info_find_method(info, native_name);

    // Find interface method
    if (!func) {
        int ifaces = g_object_info_get_n_interfaces(info);
        for (int i = 0; i < ifaces; i++) {
            GIInterfaceInfo *iface_info = g_object_info_get_interface(info, i);
            func = g_interface_info_find_method(iface_info, native_name);
            if (func) {
                g_base_info_unref(iface_info);
                return func;
            }
            g_base_info_unref(iface_info);
        }
    }

    if (!func) {
        GIObjectInfo *parent = g_object_info_get_parent(info);
        func = FindMethod(parent, js_name);
        g_base_info_unref(parent);
    }
    return func;
}

GIPropertyInfo *g_object_info_find_property(GIObjectInfo *info, char *name)
{
    int l = g_object_info_get_n_properties(info);
    for (int i=0; i<l; i++) {
        GIPropertyInfo *prop = g_object_info_get_property(info, i);
        if (strcmp(g_base_info_get_name(prop), name) == 0) {
            return prop;
        }
        g_base_info_unref(prop);
    }
    return nullptr;
}

GIPropertyInfo *GIRObject::FindProperty(GIObjectInfo *inf, char *name)
{
    GIPropertyInfo *prop = g_object_info_find_property(inf, name);
    if (!prop) {
        GIObjectInfo *parent = g_object_info_get_parent(inf);
        if (!parent)
            return nullptr;
        if (parent != inf) {
        //if (strcmp( g_base_info_get_name(parent), g_base_info_get_name(inf) ) != 0) {
            prop = FindProperty(parent, name);
        }
        g_base_info_unref(parent);
    }
    return prop;
}

GIInterfaceInfo *g_object_info_find_interface(GIObjectInfo *info, char *name)
{
    int l = g_object_info_get_n_interfaces(info);
    for (int i=0; i<l; i++) {
        GIInterfaceInfo *interface = g_object_info_get_interface(info, i);
        if (strcmp(g_base_info_get_name(interface), name) == 0) {
            return interface;
        }
        g_base_info_unref(interface);
    }
    return nullptr;
}

GIInterfaceInfo *GIRObject::FindInterface(GIObjectInfo *inf, char *name)
{
    GIInterfaceInfo *interface = g_object_info_find_interface(inf, name);
    if (!interface) {
        GIObjectInfo *parent = g_object_info_get_parent(inf);
        if (strcmp( g_base_info_get_name(parent), g_base_info_get_name(inf) ) != 0) {
            interface = FindInterface(parent, name);
        }
        g_base_info_unref(parent);
    }
    return interface;
}

GIFieldInfo *g_object_info_find_field(GIObjectInfo *info, char *name)
{
    int l = g_object_info_get_n_fields(info);
    for (int i=0; i<l; i++) {
        GIFieldInfo *field = g_object_info_get_field(info, i);
        if (strcmp(g_base_info_get_name(field), name) == 0) {
            return field;
        }
        g_base_info_unref(field);
    }
    return nullptr;
}

GIFieldInfo *GIRObject::FindField(GIObjectInfo *inf, char *name)
{
    GIFieldInfo *field = g_object_info_find_field(inf, name);
    if (!field) {
        GIObjectInfo *parent = g_object_info_get_parent(inf);
        if (strcmp( g_base_info_get_name(parent), g_base_info_get_name(inf) ) != 0) {
            field = FindField(parent, name);
        }
        g_base_info_unref(parent);
    }
    return field;
}

static GISignalInfo*
_object_info_find_interface_signal(GIObjectInfo *inf, char *name)
{
    int ifaces = g_object_info_get_n_interfaces(inf);
    GISignalInfo *signal = nullptr;
    for (int i = 0; i < ifaces; i++) {
        GIInterfaceInfo *iface_info = g_object_info_get_interface(inf, i);
        int n_signals = g_interface_info_get_n_signals(iface_info);
        for (int n = 0; n < n_signals; n++) {
            signal = g_interface_info_get_signal(iface_info, n);
            if (g_str_equal(g_base_info_get_name(signal), name)) {
                break;
            }
            g_base_info_unref(signal);
        }
        g_base_info_unref(iface_info);
    }

    return signal;
}

GISignalInfo *GIRObject::FindSignal(GIObjectInfo *inf, char *name)
{
    GISignalInfo *signal = g_object_info_find_signal(inf, name);
    if (!signal) {
        GIObjectInfo *parent = g_object_info_get_parent(inf);
        if (parent) {
            if (strcmp( g_base_info_get_name(parent), g_base_info_get_name(inf) ) != 0) {
                signal = FindSignal(parent, name);
            }
            g_base_info_unref(parent);
        }
        if (!signal)
            signal = _object_info_find_interface_signal(inf, name);
    }
    return signal;
}

GIVFuncInfo *GIRObject::FindVFunc(GIObjectInfo *inf, char *name)
{
    GISignalInfo *vfunc = g_object_info_find_vfunc(inf, name);
    if (!vfunc) {
        GIObjectInfo *parent = g_object_info_get_parent(inf);
        if (strcmp( g_base_info_get_name(parent), g_base_info_get_name(inf) ) != 0) {
            vfunc = FindVFunc(parent, name);
        }
        g_base_info_unref(parent);
    }
    return vfunc;
}

Local<v8::ObjectTemplate> GIRObject::PropertyList(GIObjectInfo *info)
{
    Local<v8::ObjectTemplate> list = Nan::New<v8::ObjectTemplate>();
    bool first = true;
    int gcounter = 0;
    g_base_info_ref(info);

    while (true) {
        if (!first) {
            GIObjectInfo *parent = g_object_info_get_parent(info);
            if (!parent) {
                return list;
            }
            if (strcmp(g_base_info_get_name(parent), g_base_info_get_name(info)) == 0) {
                return list;
            }
            g_base_info_unref(info);
            info = parent;
        }

        int l = g_object_info_get_n_properties(info);
        for (int i=0; i<l; i++) {
            GIPropertyInfo *prop = g_object_info_get_property(info, i);
            list->Set(Nan::New(std::to_string(i+gcounter)).ToLocalChecked(), Nan::New<String>(g_base_info_get_name(prop)).ToLocalChecked());
            g_base_info_unref(prop);
        }
        gcounter += l;
        first = false;
    }

    return list;
}

Handle<ObjectTemplate> GIRObject::MethodList(GIObjectInfo *info)
{
    Handle<ObjectTemplate> list = Nan::New<ObjectTemplate>();
    bool first = true;
    int gcounter = 0;
    g_base_info_ref(info);

    while (true) {
        if (!first) {
            GIObjectInfo *parent = g_object_info_get_parent(info);
            if (!parent) {
                return list;
            }
            if (strcmp( g_base_info_get_name(parent), g_base_info_get_name(info) ) == 0) {
                return list;
            }
            g_base_info_unref(info);
            info = parent;
        }

        int l = g_object_info_get_n_methods(info);
        for (int i=0; i<l; i++) {
            GIFunctionInfo *func = g_object_info_get_method(info, i);
            list->Set(Nan::New(std::to_string(i+gcounter)).ToLocalChecked(), Nan::New<String>(g_base_info_get_name(func)).ToLocalChecked());
            g_base_info_unref(func);
        }
        gcounter += l;
        first = false;
    }

    return list;
}

void GIRObject::RegisterMethods(Handle<Object> target, GIObjectInfo *info, const char *namespace_, Handle<FunctionTemplate> t)
{
    bool is_object_info = GI_IS_OBJECT_INFO(info);
    // Register interface methods
    if (is_object_info) {
        int ifaces = g_object_info_get_n_interfaces(info);
        for (int i = 0; i < ifaces; i++) {
            GIInterfaceInfo *iface_info = g_object_info_get_interface(info, i);
            // Register prerequisities
            int n_pre = g_interface_info_get_n_prerequisites(iface_info);
            for (int j = 0; j < n_pre; j++) {
                GIBaseInfo *pre_info = g_interface_info_get_prerequisite(iface_info, j);
                GIRObject::RegisterMethods(target, pre_info, namespace_, t);
                g_base_info_unref(pre_info);
            }
            GIRObject::RegisterMethods(target, iface_info, namespace_, t);
            g_base_info_unref(iface_info);
        }
    }

    bool first = true;
    int gcounter = 0;
    g_base_info_ref(info);

    while (true) {
        if (!first) {
            GIObjectInfo *parent = nullptr;
            if (GI_IS_OBJECT_INFO(info))
                parent = g_object_info_get_parent(info);
            if (!parent) {
                g_base_info_unref(info);
                return;
            }
            if (g_base_info_equal(parent, info)) {
                g_base_info_unref(info);
                return;
            }
            g_base_info_unref(info);
            info = parent;
        }

        int l = 0;
        if (is_object_info) {
            l = g_object_info_get_n_methods(info);
        } else {
            l = g_interface_info_get_n_methods(info);
        }

        for (int i=0; i<l; i++) {
            GIFunctionInfo *func = nullptr;
            if (is_object_info) {
                func = g_object_info_get_method(info, i);
            } else {
                func = g_interface_info_get_method(info, i);
            }
            GIRObject::SetMethod(t, *func);
            g_base_info_unref(func);
        }
        gcounter += l;
        first = false;
    }
}

/**
 * this function will use the GIFunctionInfo (which describes a native function)
 * to define either a static or prototype method on the target, depending on the
 * flags of the GIFunctionInfo.
 * It will also apply a snake_case to camelCase conversion to function name.
 */
void GIRObject::SetMethod(Local<FunctionTemplate> &target, GIFunctionInfo &function_info)
{
    const char *native_name = g_base_info_get_name(&function_info);
    const char *js_name = Util::toCamelCase(std::string(native_name)).c_str();

    GIFunctionInfoFlags flags = g_function_info_get_flags(&function_info);
    if (flags & GI_FUNCTION_IS_METHOD) {
        // if the function is a method, then we want to set it on the prototype
        // of the target, as a GI_FUNCTION_IS_METHOD is an instance method.
        Nan::SetPrototypeMethod(target, js_name, CallUnknownMethod);
    } else {
        // else if it's not a method, then we want to set it as a static function
        // on the target itself (not the prototype)
        Local<FunctionTemplate> static_function = Nan::New<FunctionTemplate>(Func::CallStaticMethod);

        // set a private value on the function. this is used to lookup the GIFunctionInfo
        // inside the Func::CallStaticMethod function. TODO: find an alternative to this design!
        Local<External> function_info_extern = Nan::New<External>((void *)g_base_info_ref(&function_info));
        Nan::SetPrivate(static_function->GetFunction(), Nan::New("GIInfo").ToLocalChecked(), function_info_extern);

        // set the function on the target.
        target->Set(Nan::New(js_name).ToLocalChecked(), static_function);
    }
}

Handle<ObjectTemplate> GIRObject::InterfaceList(GIObjectInfo *info)
{
    Handle<ObjectTemplate> list = Nan::New<ObjectTemplate>();
    bool first = true;
    int gcounter = 0;
    g_base_info_ref(info);

    while (true) {
        if (!first) {
            GIObjectInfo *parent = g_object_info_get_parent(info);
            if (!parent) {
                return list;
            }
            if (strcmp( g_base_info_get_name(parent), g_base_info_get_name(info) ) == 0) {
                return list;
            }
            g_base_info_unref(info);
            info = parent;
        }

        int l = g_object_info_get_n_interfaces(info);
        for (int i=0; i<l; i++) {
            GIInterfaceInfo *interface = g_object_info_get_interface(info, i);
            list->Set(Nan::New(std::to_string(i+gcounter)).ToLocalChecked(), Nan::New<String>(g_base_info_get_name(interface)).ToLocalChecked());
            g_base_info_unref(interface);
        }
        gcounter += l;
        first = false;
    }

    return list;
}

Handle<ObjectTemplate> GIRObject::FieldList(GIObjectInfo *info)
{
    Handle<ObjectTemplate> list = Nan::New<ObjectTemplate>();
    bool first = true;
    int gcounter = 0;
    g_base_info_ref(info);

    while (true) {
        if (!first) {
            GIObjectInfo *parent = g_object_info_get_parent(info);
            if (!parent) {
                return list;
            }
            if (strcmp( g_base_info_get_name(parent), g_base_info_get_name(info) ) == 0) {
                return list;
            }
            g_base_info_unref(info);
            info = parent;
        }

        int l = g_object_info_get_n_fields(info);
        for (int i=0; i<l; i++) {
            GIFieldInfo *field = g_object_info_get_field(info, i);
            list->Set(Nan::New(std::to_string(i+gcounter)).ToLocalChecked(), Nan::New<String>(g_base_info_get_name(field)).ToLocalChecked());
            g_base_info_unref(field);
        }
        gcounter += l;
        first = false;
    }

    return list;
}

Handle<ObjectTemplate> GIRObject::SignalList(GIObjectInfo *info)
{
    Handle<ObjectTemplate> list = Nan::New<ObjectTemplate>();
    bool first = true;
    int gcounter = 0;
    g_base_info_ref(info);

    while (true) {
        if (!first) {
            GIObjectInfo *parent = g_object_info_get_parent(info);
            if (!parent) {
                return list;
            }
            if (strcmp( g_base_info_get_name(parent), g_base_info_get_name(info)/*"InitiallyUnowned"*/ ) == 0) {
                return list;
            }
            g_base_info_unref(info);
            info = parent;
        }

        int l = g_object_info_get_n_signals(info);
        for (int i=0; i<l; i++) {
            GISignalInfo *signal = g_object_info_get_signal(info, i);
            list->Set(Nan::New(std::to_string(i+gcounter)).ToLocalChecked(), Nan::New<String>(g_base_info_get_name(signal)).ToLocalChecked());
            g_base_info_unref(signal);
        }
        gcounter += l;
        first = false;
    }

    return list;
}

Handle<ObjectTemplate> GIRObject::VFuncList(GIObjectInfo *info)
{
    Handle<ObjectTemplate> list = Nan::New<ObjectTemplate>();
    bool first = true;
    int gcounter = 0;
    g_base_info_ref(info);

    while (true) {
        if (!first) {
            GIObjectInfo *parent = g_object_info_get_parent(info);
            if (!parent) {
                return list;
            }
            if (strcmp( g_base_info_get_name(parent), g_base_info_get_name(info) ) == 0) {
                return list;
            }
            g_base_info_unref(info);
            info = parent;
        }

        int l = g_object_info_get_n_vfuncs(info);
        for (int i=0; i<l; i++) {
            GIVFuncInfo *vfunc = g_object_info_get_vfunc(info, i);
            list->Set(Nan::New(std::to_string(i+gcounter)).ToLocalChecked(), Nan::New<String>(g_base_info_get_name(vfunc)).ToLocalChecked());
            g_base_info_unref(vfunc);
        }
        gcounter += l;
        first = false;
    }

    return list;
}

}
