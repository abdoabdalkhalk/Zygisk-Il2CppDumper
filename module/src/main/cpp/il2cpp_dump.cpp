#include "il2cpp_dump.h"
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <cinttypes>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <unistd.h>
#include "xdl.h"
#include "log.h"
#include "il2cpp-tabledefs.h"
#include "il2cpp-class.h"

#define DO_API(r, n, p) r (*n) p
#include "il2cpp-api-functions.h"
#undef DO_API

static uint64_t il2cpp_base = 0;

void init_il2cpp_api(void *handle) {
#define DO_API(r, n, p) {                      \
    n = (r (*) p)xdl_sym(handle, #n, nullptr); \
    if(!n) {                                   \
        LOGW("api not found %s", #n);          \
    }                                          \
}
#include "il2cpp-api-functions.h"
#undef DO_API
}

static std::string get_type_name(const Il2CppType *type, bool include_namespace = false);

static const char* primitive_type_name(int type_enum) {
    switch (type_enum) {
        case IL2CPP_TYPE_VOID:       return "void";
        case IL2CPP_TYPE_BOOLEAN:    return "bool";
        case IL2CPP_TYPE_CHAR:       return "char";
        case IL2CPP_TYPE_I1:         return "sbyte";
        case IL2CPP_TYPE_U1:         return "byte";
        case IL2CPP_TYPE_I2:         return "short";
        case IL2CPP_TYPE_U2:         return "ushort";
        case IL2CPP_TYPE_I4:         return "int";
        case IL2CPP_TYPE_U4:         return "uint";
        case IL2CPP_TYPE_I8:         return "long";
        case IL2CPP_TYPE_U8:         return "ulong";
        case IL2CPP_TYPE_R4:         return "float";
        case IL2CPP_TYPE_R8:         return "double";
        case IL2CPP_TYPE_STRING:     return "string";
        case IL2CPP_TYPE_OBJECT:     return "object";
        case IL2CPP_TYPE_TYPEDBYREF: return "TypedReference";
        case IL2CPP_TYPE_I:          return "IntPtr";
        case IL2CPP_TYPE_U:          return "UIntPtr";
        default:                     return nullptr;
    }
}

static std::string get_class_name_clean(Il2CppClass *klass) {
    if (!klass) return "?";
    const char *name = il2cpp_class_get_name(klass);
    if (!name) return "?";
    std::string s(name);
    size_t tick = s.find('`');
    if (tick != std::string::npos) s = s.substr(0, tick);
    return s;
}

static std::string get_class_name_with_namespace(Il2CppClass *klass) {
    if (!klass) return "?";
    const char *ns = il2cpp_class_get_namespace(klass);
    std::string name = get_class_name_clean(klass);
    if (ns && strlen(ns) > 0) return std::string(ns) + "." + name;
    return name;
}

static std::string map_system_type(const std::string &fullname) {
    if (fullname == "System.Void")    return "void";
    if (fullname == "System.Boolean") return "bool";
    if (fullname == "System.Char")    return "char";
    if (fullname == "System.SByte")   return "sbyte";
    if (fullname == "System.Byte")    return "byte";
    if (fullname == "System.Int16")   return "short";
    if (fullname == "System.UInt16")  return "ushort";
    if (fullname == "System.Int32")   return "int";
    if (fullname == "System.UInt32")  return "uint";
    if (fullname == "System.Int64")   return "long";
    if (fullname == "System.UInt64")  return "ulong";
    if (fullname == "System.Single")  return "float";
    if (fullname == "System.Double")  return "double";
    if (fullname == "System.String")  return "string";
    if (fullname == "System.Object")  return "object";
    if (fullname == "System.IntPtr")  return "IntPtr";
    if (fullname == "System.UIntPtr") return "UIntPtr";
    return "";
}

static std::string get_type_name(const Il2CppType *type, bool include_namespace) {
    if (!type) return "?";

    const char *prim = primitive_type_name(type->type);
    if (prim) return prim;

    switch (type->type) {

        case IL2CPP_TYPE_SZARRAY: {
            const Il2CppType *elem = (const Il2CppType *)type->data.type;
            if (elem) return get_type_name(elem) + "[]";
            Il2CppClass *k = il2cpp_class_from_type(type);
            if (!k) return "?[]";
            Il2CppClass *e = il2cpp_class_get_element_class(k);
            if (!e) return "?[]";
            return get_type_name(il2cpp_class_get_type(e)) + "[]";
        }

        case IL2CPP_TYPE_ARRAY: {
            Il2CppArrayType *at = type->data.array;
            if (at && at->etype) {
                std::string r = get_type_name(at->etype);
                r += "[";
                for (int i = 1; i < at->rank; i++) r += ",";
                r += "]";
                return r;
            }
            Il2CppClass *k = il2cpp_class_from_type(type);
            if (!k) return "?";
            Il2CppClass *e = il2cpp_class_get_element_class(k);
            if (!e) return "?";
            int rank = il2cpp_class_get_rank(k);
            std::string r = get_type_name(il2cpp_class_get_type(e));
            r += "[";
            for (int i = 1; i < rank; i++) r += ",";
            r += "]";
            return r;
        }

        case IL2CPP_TYPE_PTR: {
            const Il2CppType *pt = (const Il2CppType *)type->data.type;
            if (pt) return get_type_name(pt) + "*";
            return "void*";
        }

        case IL2CPP_TYPE_GENERICINST: {
            Il2CppGenericClass *gclass = type->data.generic_class;
            if (!gclass) break;

            const Il2CppType *gtype = gclass->type;
            Il2CppClass *def_klass = nullptr;
            if (gtype) def_klass = il2cpp_class_from_type(gtype);
            if (!def_klass) def_klass = il2cpp_class_from_type(type);
            if (!def_klass) return "?";

            std::string fullname = get_class_name_with_namespace(def_klass);
            std::string mapped = map_system_type(fullname);
            std::string base_name;
            if (!mapped.empty()) {
                base_name = mapped;
            } else {
                base_name = get_class_name_clean(def_klass);
            }

            const Il2CppGenericInst *inst = gclass->context.class_inst;
            if (inst && inst->type_argc > 0 && inst->type_argv) {
                base_name += "<";
                for (uint32_t i = 0; i < inst->type_argc; i++) {
                    if (i > 0) base_name += ", ";
                    const Il2CppType *arg_type = inst->type_argv[i];
                    if (arg_type) {
                        base_name += get_type_name(arg_type);
                    } else {
                        base_name += "?";
                    }
                }
                base_name += ">";
            }
            return base_name;
        }

        case IL2CPP_TYPE_VAR:
        case IL2CPP_TYPE_MVAR: {
            Il2CppClass *k = il2cpp_class_from_type(type);
            if (k) {
                const char *n = il2cpp_class_get_name(k);
                if (n) return n;
            }
            return "T";
        }

        case IL2CPP_TYPE_CLASS:
        case IL2CPP_TYPE_VALUETYPE: {
            Il2CppClass *k = il2cpp_class_from_type(type);
            if (!k) return "?";
            std::string fullname = get_class_name_with_namespace(k);
            std::string mapped = map_system_type(fullname);
            if (!mapped.empty()) return mapped;
            if (include_namespace) return fullname;
            return get_class_name_clean(k);
        }

        default:
            break;
    }

    Il2CppClass *k = il2cpp_class_from_type(type);
    if (!k) return "?";
    std::string fullname = get_class_name_with_namespace(k);
    std::string mapped = map_system_type(fullname);
    if (!mapped.empty()) return mapped;
    return get_class_name_clean(k);
}

std::string get_method_modifier(uint32_t flags) {
    std::stringstream ss;
    switch (flags & METHOD_ATTRIBUTE_MEMBER_ACCESS_MASK) {
        case METHOD_ATTRIBUTE_PRIVATE:       ss << "private "; break;
        case METHOD_ATTRIBUTE_PUBLIC:        ss << "public "; break;
        case METHOD_ATTRIBUTE_FAMILY:        ss << "protected "; break;
        case METHOD_ATTRIBUTE_ASSEM:
        case METHOD_ATTRIBUTE_FAM_AND_ASSEM: ss << "internal "; break;
        case METHOD_ATTRIBUTE_FAM_OR_ASSEM:  ss << "protected internal "; break;
    }
    if (flags & METHOD_ATTRIBUTE_STATIC) ss << "static ";
    if (flags & METHOD_ATTRIBUTE_ABSTRACT) {
        ss << "abstract ";
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_REUSE_SLOT)
            ss << "override ";
    } else if (flags & METHOD_ATTRIBUTE_FINAL) {
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_REUSE_SLOT)
            ss << "sealed override ";
    } else if (flags & METHOD_ATTRIBUTE_VIRTUAL) {
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_NEW_SLOT)
            ss << "virtual ";
        else
            ss << "override ";
    }
    if (flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) ss << "extern ";
    return ss.str();
}

bool _il2cpp_type_is_byref(const Il2CppType *type) {
    if (il2cpp_type_is_byref) return il2cpp_type_is_byref(type);
    return type->byref;
}

std::string dump_method(Il2CppClass *klass) {
    std::stringstream out;
    out << "\n\t// Methods\n";
    void *iter = nullptr;
    while (auto method = il2cpp_class_get_methods(klass, &iter)) {
        if (method->methodPointer) {
            out << "\t// RVA: 0x" << std::hex << ((uint64_t)method->methodPointer - il2cpp_base)
                << " VA: 0x" << (uint64_t)method->methodPointer;
        } else {
            out << "\t// RVA: 0x VA: 0x0";
        }
        out << "\n\t";
        uint32_t iflags = 0;
        uint32_t flags = il2cpp_method_get_flags(method, &iflags);
        out << get_method_modifier(flags);
        auto ret = il2cpp_method_get_return_type(method);
        if (_il2cpp_type_is_byref(ret)) out << "ref ";
        out << get_type_name(ret) << " " << il2cpp_method_get_name(method) << "(";
        int pc = il2cpp_method_get_param_count(method);
        for (int i = 0; i < pc; i++) {
            auto p = il2cpp_method_get_param(method, i);
            auto a = p->attrs;
            if (_il2cpp_type_is_byref(p)) {
                if (a & PARAM_ATTRIBUTE_OUT && !(a & PARAM_ATTRIBUTE_IN)) out << "out ";
                else if (a & PARAM_ATTRIBUTE_IN && !(a & PARAM_ATTRIBUTE_OUT)) out << "in ";
                else out << "ref ";
            } else {
                if (a & PARAM_ATTRIBUTE_IN) out << "[In] ";
                if (a & PARAM_ATTRIBUTE_OUT) out << "[Out] ";
            }
            out << get_type_name(p) << " " << il2cpp_method_get_param_name(method, i);
            if (i < pc - 1) out << ", ";
        }
        out << ") { }\n";
    }
    return out.str();
}

std::string dump_property(Il2CppClass *klass) {
    std::stringstream out;
    out << "\n\t// Properties\n";
    void *iter = nullptr;
    while (auto prop = il2cpp_class_get_properties(klass, &iter)) {
        auto get = il2cpp_property_get_get_method(prop);
        auto set = il2cpp_property_get_set_method(prop);
        const char *name = il2cpp_property_get_name(prop);
        out << "\t";
        uint32_t iflags = 0;
        if (get) {
            out << get_method_modifier(il2cpp_method_get_flags(get, &iflags));
            out << get_type_name(il2cpp_method_get_return_type(get)) << " " << name << " { ";
            out << "get; ";
            if (set) out << "set; ";
        } else if (set) {
            out << get_method_modifier(il2cpp_method_get_flags(set, &iflags));
            out << get_type_name(il2cpp_method_get_param(set, 0)) << " " << name << " { ";
            out << "set; ";
        }
        out << "}\n";
    }
    return out.str();
}

std::string dump_field(Il2CppClass *klass) {
    std::stringstream out;
    out << "\n\t// Fields\n";
    bool is_enum = il2cpp_class_is_enum(klass);
    void *iter = nullptr;
    while (auto field = il2cpp_class_get_fields(klass, &iter)) {
        out << "\t";
        uint32_t attrs = il2cpp_field_get_flags(field);
        switch (attrs & FIELD_ATTRIBUTE_FIELD_ACCESS_MASK) {
            case FIELD_ATTRIBUTE_PRIVATE: out << "private "; break;
            case FIELD_ATTRIBUTE_PUBLIC:  out << "public "; break;
            case FIELD_ATTRIBUTE_FAMILY:  out << "protected "; break;
            case FIELD_ATTRIBUTE_ASSEMBLY:
            case FIELD_ATTRIBUTE_FAM_AND_ASSEM: out << "internal "; break;
            case FIELD_ATTRIBUTE_FAM_OR_ASSEM:  out << "protected internal "; break;
        }
        if (attrs & FIELD_ATTRIBUTE_LITERAL) out << "const ";
        else {
            if (attrs & FIELD_ATTRIBUTE_STATIC) out << "static ";
            if (attrs & FIELD_ATTRIBUTE_INIT_ONLY) out << "readonly ";
        }
        out << get_type_name(il2cpp_field_get_type(field)) << " " << il2cpp_field_get_name(field);
        if ((attrs & FIELD_ATTRIBUTE_LITERAL) && is_enum) {
            uint64_t val = 0;
            il2cpp_field_static_get_value(field, &val);
            out << " = " << std::dec << val;
        }
        out << "; // 0x" << std::hex << il2cpp_field_get_offset(field) << "\n";
    }
    return out.str();
}

std::string dump_type(const Il2CppType *type, int typeDefIndex) {
    std::stringstream out;
    auto *klass = il2cpp_class_from_type(type);
    out << "\n// Namespace: " << il2cpp_class_get_namespace(klass) << "\n";
    uint32_t flags = il2cpp_class_get_flags(klass);
    if (flags & TYPE_ATTRIBUTE_SERIALIZABLE) out << "[Serializable]\n";
    bool vt = il2cpp_class_is_valuetype(klass);
    bool en = il2cpp_class_is_enum(klass);
    switch (flags & TYPE_ATTRIBUTE_VISIBILITY_MASK) {
        case TYPE_ATTRIBUTE_PUBLIC:
        case TYPE_ATTRIBUTE_NESTED_PUBLIC:       out << "public "; break;
        case TYPE_ATTRIBUTE_NOT_PUBLIC:
        case TYPE_ATTRIBUTE_NESTED_FAM_AND_ASSEM:
        case TYPE_ATTRIBUTE_NESTED_ASSEMBLY:     out << "internal "; break;
        case TYPE_ATTRIBUTE_NESTED_PRIVATE:      out << "private "; break;
        case TYPE_ATTRIBUTE_NESTED_FAMILY:       out << "protected "; break;
        case TYPE_ATTRIBUTE_NESTED_FAM_OR_ASSEM: out << "protected internal "; break;
    }
    if (flags & TYPE_ATTRIBUTE_ABSTRACT && flags & TYPE_ATTRIBUTE_SEALED) out << "static ";
    else if (!(flags & TYPE_ATTRIBUTE_INTERFACE) && flags & TYPE_ATTRIBUTE_ABSTRACT) out << "abstract ";
    else if (!vt && !en && flags & TYPE_ATTRIBUTE_SEALED) out << "sealed ";
    if (flags & TYPE_ATTRIBUTE_INTERFACE) out << "interface ";
    else if (en) out << "enum ";
    else if (vt) out << "struct ";
    else out << "class ";

    out << get_type_name(type);

    std::vector<std::string> extends;
    auto parent = il2cpp_class_get_parent(klass);
    if (!vt && !en && parent) {
        auto pt = il2cpp_class_get_type(parent);
        if (pt->type != IL2CPP_TYPE_OBJECT)
            extends.push_back(get_type_name(pt));
    }
    void *iter = nullptr;
    while (auto itf = il2cpp_class_get_interfaces(klass, &iter))
        extends.push_back(get_type_name(il2cpp_class_get_type(itf)));
    if (!extends.empty()) {
        out << " : " << extends[0];
        for (size_t i = 1; i < extends.size(); i++) out << ", " << extends[i];
    }
    out << " // TypeDefIndex: " << std::dec << typeDefIndex << "\n{";
    out << dump_field(klass);
    out << dump_property(klass);
    out << dump_method(klass);
    out << "}\n";
    return out.str();
}

void il2cpp_api_init(void *handle) {
    LOGI("il2cpp_handle: %p", handle);
    init_il2cpp_api(handle);
    if (il2cpp_domain_get_assemblies) {
        Dl_info dlInfo;
        if (dladdr((void *)il2cpp_domain_get_assemblies, &dlInfo))
            il2cpp_base = (uint64_t)dlInfo.dli_fbase;
        LOGI("il2cpp_base: %" PRIx64, il2cpp_base);
    }
    while (!il2cpp_is_vm_thread(nullptr)) sleep(1);
    il2cpp_thread_attach(il2cpp_domain_get());
}

void il2cpp_dump(const char *outDir) {
    LOGI("dumping...");
    size_t size;
    auto domain = il2cpp_domain_get();
    auto assemblies = il2cpp_domain_get_assemblies(domain, &size);
    std::stringstream header;
    for (int i = 0; i < (int)size; i++)
        header << "// Image " << i << ": " << il2cpp_image_get_name(il2cpp_assembly_get_image(assemblies[i])) << "\n";

    std::vector<std::string> lines;
    int idx = 0;
    if (il2cpp_image_get_class) {
        for (int i = 0; i < (int)size; i++) {
            auto img = il2cpp_assembly_get_image(assemblies[i]);
            std::string ih = std::string("\n// Dll : ") + il2cpp_image_get_name(img);
            for (int j = 0; j < (int)il2cpp_image_get_class_count(img); j++)
                lines.push_back(ih + dump_type(il2cpp_class_get_type(il2cpp_image_get_class(img, j)), idx++));
        }
    }
    std::string path = std::string(outDir) + "/files/dump.cs";
    std::ofstream f(path);
    f << header.str();
    for (auto &l : lines) f << l;
    f.close();
    LOGI("dump done!");
}
