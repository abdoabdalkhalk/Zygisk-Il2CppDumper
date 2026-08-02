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

// strips trailing unmatched '>' from obfuscated class names
// e.g. "OBHKEJHFBIP>" -> "OBHKEJHFBIP"
// but "List<int>" stays "List<int>"
static std::string strip_unmatched_brackets(const std::string &name) {
    int open = 0;
    for (char c : name) {
        if (c == '<') open++;
        else if (c == '>') open--;
    }
    // open < 0 means more '>' than '<' -> strip trailing '>'
    if (open >= 0) return name;
    std::string result = name;
    int extra = -open;
    for (int i = (int)result.size() - 1; i >= 0 && extra > 0; i--) {
        if (result[i] == '>') {
            result.erase(i, 1);
            extra--;
        }
    }
    return result;
}

static const char* primitive_type_name(Il2CppTypeEnum type) {
    switch (type) {
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
        case IL2CPP_TYPE_I:          return "IntPtr";
        case IL2CPP_TYPE_U:          return "UIntPtr";
        case IL2CPP_TYPE_OBJECT:     return "object";
        case IL2CPP_TYPE_TYPEDBYREF: return "TypedReference";
        default:                     return nullptr;
    }
}

static std::string get_type_name(const Il2CppType *type);

static std::string get_class_name_no_tick(Il2CppClass *klass) {
    const char *raw = il2cpp_class_get_name(klass);
    if (!raw) return "?";
    std::string name(raw);
    auto tick = name.find('`');
    if (tick != std::string::npos) {
        name = name.substr(0, tick);
    }
    return strip_unmatched_brackets(name);
}

// converts a single dotnet type token (from il2cpp_type_get_name output) to C# name
// input examples: "System.UInt32", "OBHKEJHFBIP", "[System.UInt32, mscorlib]"
static std::string dotnet_token_to_csharp(const std::string &token) {
    static const struct { const char *dotnet; const char *csharp; } map[] = {
        {"System.Void",    "void"},
        {"System.Boolean", "bool"},
        {"System.Char",    "char"},
        {"System.SByte",   "sbyte"},
        {"System.Byte",    "byte"},
        {"System.Int16",   "short"},
        {"System.UInt16",  "ushort"},
        {"System.Int32",   "int"},
        {"System.UInt32",  "uint"},
        {"System.Int64",   "long"},
        {"System.UInt64",  "ulong"},
        {"System.Single",  "float"},
        {"System.Double",  "double"},
        {"System.String",  "string"},
        {"System.Object",  "object"},
        {"System.IntPtr",  "IntPtr"},
        {"System.UIntPtr", "UIntPtr"},
        {nullptr, nullptr}
    };

    std::string t = token;

    // trim leading spaces
    while (!t.empty() && t[0] == ' ') t = t.substr(1);

    // unwrap "[TypeName, Assembly]" -> "TypeName"
    if (!t.empty() && t[0] == '[') {
        // find the matching closing bracket at depth 1
        // but if it's a nested generic like "[Outer`1[[Inner, Asm]], Asm]"
        // we only want the first comma at depth 1
        int depth = 0;
        size_t comma_pos = std::string::npos;
        for (size_t i = 0; i < t.size(); i++) {
            if (t[i] == '[') depth++;
            else if (t[i] == ']') depth--;
            else if (t[i] == ',' && depth == 1) {
                comma_pos = i;
                break;
            }
        }
        if (comma_pos != std::string::npos)
            t = t.substr(1, comma_pos - 1);
        else
            t = t.substr(1, t.size() - 2);
        while (!t.empty() && t[0] == ' ') t = t.substr(1);
    }

    // check dotnet -> csharp map
    for (int i = 0; map[i].dotnet; i++) {
        if (t == map[i].dotnet) return map[i].csharp;
    }

    // strip namespace: take part after last '.' before any '`' or '<'
    auto generic_start = t.find_first_of("`<");
    std::string prefix = (generic_start != std::string::npos) ? t.substr(0, generic_start) : t;
    auto last_dot = prefix.rfind('.');
    if (last_dot != std::string::npos) {
        t = t.substr(last_dot + 1);
    }

    // strip backtick
    auto tick = t.find('`');
    if (tick != std::string::npos) t = t.substr(0, tick);

    return strip_unmatched_brackets(t);
}

// parse comma-separated type args from il2cpp_type_get_name output
// handles nested brackets: "[Outer`1[[Inner, Asm],[Inner2,Asm]], Asm], [Other, Asm]"
static std::vector<std::string> split_type_args(const std::string &args_raw) {
    std::vector<std::string> result;
    int depth = 0;
    std::string cur;
    for (char c : args_raw) {
        if      (c == '[') { depth++; cur += c; }
        else if (c == ']') { depth--; cur += c; }
        else if (c == ',' && depth == 0) {
            while (!cur.empty() && cur[0] == ' ') cur = cur.substr(1);
            if (!cur.empty()) result.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    while (!cur.empty() && cur[0] == ' ') cur = cur.substr(1);
    if (!cur.empty()) result.push_back(cur);
    return result;
}

static std::string get_type_name(const Il2CppType *type) {
    if (!type) return "?";

    const char *prim = primitive_type_name(type->type);
    if (prim) return prim;

    Il2CppClass *klass = nullptr;

    switch ((int)type->type) {

        case IL2CPP_TYPE_SZARRAY: {
            if (!il2cpp_class_from_type) break;
            klass = il2cpp_class_from_type(type);
            if (!klass) break;
            Il2CppClass *elem = il2cpp_class_get_element_class(klass);
            if (!elem) break;
            return get_type_name(il2cpp_class_get_type(elem)) + "[]";
        }

        case IL2CPP_TYPE_ARRAY: {
            if (!il2cpp_class_from_type) break;
            klass = il2cpp_class_from_type(type);
            if (!klass) break;
            Il2CppClass *elem = il2cpp_class_get_element_class(klass);
            if (!elem) break;
            int rank = il2cpp_class_get_rank(klass);
            std::string r = get_type_name(il2cpp_class_get_type(elem)) + "[";
            for (int i = 1; i < rank; i++) r += ",";
            return r + "]";
        }

        case IL2CPP_TYPE_PTR: {
            if (!il2cpp_class_from_type) break;
            klass = il2cpp_class_from_type(type);
            if (!klass) break;
            Il2CppClass *elem = il2cpp_class_get_element_class(klass);
            if (!elem) break;
            return get_type_name(il2cpp_class_get_type(elem)) + "*";
        }

        case IL2CPP_TYPE_GENERICINST: {
            // try il2cpp_type_get_name first — gives us full dotnet name with args
            if (il2cpp_type_get_name) {
                char *full = il2cpp_type_get_name(type);
                if (full) {
                    std::string s(full);
                    il2cpp_free(full);

                    // format: "Namespace.ClassName`N[arg1,arg2,...]"
                    // or:     "Namespace.ClassName`N[[arg1,Asm],[arg2,Asm]]"

                    // extract class name (after last dot, before backtick)
                    auto tick_pos = s.find('`');
                    std::string class_part;
                    if (tick_pos != std::string::npos) {
                        auto dot_pos = s.rfind('.', tick_pos);
                        class_part = (dot_pos != std::string::npos)
                            ? s.substr(dot_pos + 1, tick_pos - dot_pos - 1)
                            : s.substr(0, tick_pos);
                    } else {
                        auto dot_pos = s.rfind('.');
                        class_part = (dot_pos != std::string::npos)
                            ? s.substr(dot_pos + 1)
                            : s;
                    }
                    class_part = strip_unmatched_brackets(class_part);

                    // find the args block "[...]"
                    // the first '[' after the backtick number
                    size_t args_open = std::string::npos;
                    if (tick_pos != std::string::npos) {
                        // skip the number after backtick
                        size_t i = tick_pos + 1;
                        while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
                        if (i < s.size() && s[i] == '[') args_open = i;
                    }

                    if (args_open != std::string::npos) {
                        // find matching closing bracket
                        int depth = 0;
                        size_t args_close = std::string::npos;
                        for (size_t i = args_open; i < s.size(); i++) {
                            if      (s[i] == '[') depth++;
                            else if (s[i] == ']') {
                                depth--;
                                if (depth == 0) { args_close = i; break; }
                            }
                        }
                        if (args_close != std::string::npos) {
                            std::string args_raw = s.substr(args_open + 1, args_close - args_open - 1);
                            auto tokens = split_type_args(args_raw);
                            std::string result = class_part + "<";
                            for (int i = 0; i < (int)tokens.size(); i++) {
                                if (i > 0) result += ", ";
                                result += dotnet_token_to_csharp(tokens[i]);
                            }
                            result += ">";
                            return result;
                        }
                    }
                    // no args found — just return class name
                    return class_part;
                }
            }

            // fallback: no il2cpp_type_get_name
            klass = il2cpp_class_from_type(type);
            if (!klass) return "?";
            return get_class_name_no_tick(klass);
        }

        case IL2CPP_TYPE_VAR:
        case IL2CPP_TYPE_MVAR: {
            klass = il2cpp_class_from_type(type);
            if (!klass) return "T";
            const char *n = il2cpp_class_get_name(klass);
            return n ? strip_unmatched_brackets(std::string(n)) : "T";
        }

        default:
            break;
    }

    if (!il2cpp_class_from_type) return "?";
    klass = il2cpp_class_from_type(type);
    if (!klass) return "?";

    const char *name = il2cpp_class_get_name(klass);
    if (!name) return "?";

    // System namespace primitives by class name
    static const struct { const char *il2cpp_name; const char *csharp_name; } name_map[] = {
        {"Void",          "void"},
        {"Boolean",       "bool"},
        {"Char",          "char"},
        {"SByte",         "sbyte"},
        {"Byte",          "byte"},
        {"Int16",         "short"},
        {"UInt16",        "ushort"},
        {"Int32",         "int"},
        {"UInt32",        "uint"},
        {"Int64",         "long"},
        {"UInt64",        "ulong"},
        {"Single",        "float"},
        {"Double",        "double"},
        {"String",        "string"},
        {"Object",        "object"},
        {"IntPtr",        "IntPtr"},
        {"UIntPtr",       "UIntPtr"},
        {nullptr, nullptr}
    };

    const char *ns = il2cpp_class_get_namespace(klass);
    if (ns && strcmp(ns, "System") == 0) {
        for (int i = 0; name_map[i].il2cpp_name; i++) {
            if (strcmp(name, name_map[i].il2cpp_name) == 0) {
                return name_map[i].csharp_name;
            }
        }
    }

    return strip_unmatched_brackets(std::string(name));
}

std::string get_method_modifier(uint32_t flags) {
    std::stringstream outPut;
    auto access = flags & METHOD_ATTRIBUTE_MEMBER_ACCESS_MASK;
    switch (access) {
        case METHOD_ATTRIBUTE_PRIVATE:           outPut << "private ";            break;
        case METHOD_ATTRIBUTE_PUBLIC:            outPut << "public ";             break;
        case METHOD_ATTRIBUTE_FAMILY:            outPut << "protected ";          break;
        case METHOD_ATTRIBUTE_ASSEM:
        case METHOD_ATTRIBUTE_FAM_AND_ASSEM:     outPut << "internal ";           break;
        case METHOD_ATTRIBUTE_FAM_OR_ASSEM:      outPut << "protected internal "; break;
    }
    if (flags & METHOD_ATTRIBUTE_STATIC) outPut << "static ";
    if (flags & METHOD_ATTRIBUTE_ABSTRACT) {
        outPut << "abstract ";
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_REUSE_SLOT)
            outPut << "override ";
    } else if (flags & METHOD_ATTRIBUTE_FINAL) {
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_REUSE_SLOT)
            outPut << "sealed override ";
    } else if (flags & METHOD_ATTRIBUTE_VIRTUAL) {
        if ((flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_NEW_SLOT)
            outPut << "virtual ";
        else
            outPut << "override ";
    }
    if (flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) outPut << "extern ";
    return outPut.str();
}

bool _il2cpp_type_is_byref(const Il2CppType *type) {
    auto byref = type->byref;
    if (il2cpp_type_is_byref) byref = il2cpp_type_is_byref(type);
    return byref;
}

std::string dump_method(Il2CppClass *klass) {
    std::stringstream outPut;
    outPut << "\n\t// Methods\n";
    void *iter = nullptr;
    while (auto method = il2cpp_class_get_methods(klass, &iter)) {
        if (method->methodPointer) {
            outPut << "\t// RVA: 0x" << std::hex << (uint64_t)method->methodPointer - il2cpp_base;
            outPut << " VA: 0x"      << std::hex << (uint64_t)method->methodPointer;
        } else {
            outPut << "\t// RVA: 0x VA: 0x0";
        }
        outPut << "\n\t";
        uint32_t iflags = 0;
        auto flags = il2cpp_method_get_flags(method, &iflags);
        outPut << get_method_modifier(flags);

        auto return_type = il2cpp_method_get_return_type(method);
        if (_il2cpp_type_is_byref(return_type)) outPut << "ref ";
        outPut << get_type_name(return_type) << " " << il2cpp_method_get_name(method) << "(";

        auto param_count = il2cpp_method_get_param_count(method);
        for (int i = 0; i < param_count; ++i) {
            auto param = il2cpp_method_get_param(method, i);
            auto attrs = param->attrs;
            if (_il2cpp_type_is_byref(param)) {
                if      (attrs & PARAM_ATTRIBUTE_OUT && !(attrs & PARAM_ATTRIBUTE_IN)) outPut << "out ";
                else if (attrs & PARAM_ATTRIBUTE_IN  && !(attrs & PARAM_ATTRIBUTE_OUT)) outPut << "in ";
                else outPut << "ref ";
            } else {
                if (attrs & PARAM_ATTRIBUTE_IN)  outPut << "[In] ";
                if (attrs & PARAM_ATTRIBUTE_OUT) outPut << "[Out] ";
            }
            outPut << get_type_name(param) << " " << il2cpp_method_get_param_name(method, i);
            outPut << ", ";
        }
        if (param_count > 0) outPut.seekp(-2, outPut.cur);
        outPut << ") { }\n";
    }
    return outPut.str();
}

std::string dump_property(Il2CppClass *klass) {
    std::stringstream outPut;
    outPut << "\n\t// Properties\n";
    void *iter = nullptr;
    while (auto prop_const = il2cpp_class_get_properties(klass, &iter)) {
        auto prop      = const_cast<PropertyInfo *>(prop_const);
        auto get       = il2cpp_property_get_get_method(prop);
        auto set       = il2cpp_property_get_set_method(prop);
        auto prop_name = il2cpp_property_get_name(prop);
        outPut << "\t";
        uint32_t iflags = 0;
        if (get) {
            outPut << get_method_modifier(il2cpp_method_get_flags(get, &iflags));
            outPut << get_type_name(il2cpp_method_get_return_type(get)) << " " << prop_name << " { ";
        } else if (set) {
            outPut << get_method_modifier(il2cpp_method_get_flags(set, &iflags));
            outPut << get_type_name(il2cpp_method_get_param(set, 0)) << " " << prop_name << " { ";
        } else {
            if (prop_name) outPut << " // unknown property " << prop_name;
            continue;
        }
        if (get) outPut << "get; ";
        if (set) outPut << "set; ";
        outPut << "}\n";
    }
    return outPut.str();
}

std::string dump_field(Il2CppClass *klass) {
    std::stringstream outPut;
    outPut << "\n\t// Fields\n";
    auto is_enum = il2cpp_class_is_enum(klass);
    void *iter   = nullptr;
    while (auto field = il2cpp_class_get_fields(klass, &iter)) {
        outPut << "\t";
        auto attrs  = il2cpp_field_get_flags(field);
        auto access = attrs & FIELD_ATTRIBUTE_FIELD_ACCESS_MASK;
        switch (access) {
            case FIELD_ATTRIBUTE_PRIVATE:            outPut << "private ";            break;
            case FIELD_ATTRIBUTE_PUBLIC:             outPut << "public ";             break;
            case FIELD_ATTRIBUTE_FAMILY:             outPut << "protected ";          break;
            case FIELD_ATTRIBUTE_ASSEMBLY:
            case FIELD_ATTRIBUTE_FAM_AND_ASSEM:      outPut << "internal ";           break;
            case FIELD_ATTRIBUTE_FAM_OR_ASSEM:       outPut << "protected internal "; break;
        }
        if (attrs & FIELD_ATTRIBUTE_LITERAL) {
            outPut << "const ";
        } else {
            if (attrs & FIELD_ATTRIBUTE_STATIC)    outPut << "static ";
            if (attrs & FIELD_ATTRIBUTE_INIT_ONLY) outPut << "readonly ";
        }

        auto field_type = il2cpp_field_get_type(field);
        outPut << get_type_name(field_type) << " " << il2cpp_field_get_name(field);

        if (attrs & FIELD_ATTRIBUTE_LITERAL && is_enum) {
            uint64_t val = 0;
            il2cpp_field_static_get_value(field, &val);
            outPut << " = " << std::dec << val;
        }
        outPut << "; // 0x" << std::hex << il2cpp_field_get_offset(field) << "\n";
    }
    return outPut.str();
}

std::string dump_type(const Il2CppType *type, int typeDefIndex) {
    std::stringstream outPut;
    auto *klass       = il2cpp_class_from_type(type);
    auto flags        = il2cpp_class_get_flags(klass);
    auto is_valuetype = il2cpp_class_is_valuetype(klass);
    auto is_enum      = il2cpp_class_is_enum(klass);

    outPut << "\n// Namespace: " << il2cpp_class_get_namespace(klass) << "\n";

    if (flags & TYPE_ATTRIBUTE_SERIALIZABLE) outPut << "[Serializable]\n";

    auto visibility = flags & TYPE_ATTRIBUTE_VISIBILITY_MASK;
    switch (visibility) {
        case TYPE_ATTRIBUTE_PUBLIC:
        case TYPE_ATTRIBUTE_NESTED_PUBLIC:        outPut << "public ";             break;
        case TYPE_ATTRIBUTE_NOT_PUBLIC:
        case TYPE_ATTRIBUTE_NESTED_FAM_AND_ASSEM:
        case TYPE_ATTRIBUTE_NESTED_ASSEMBLY:      outPut << "internal ";           break;
        case TYPE_ATTRIBUTE_NESTED_PRIVATE:       outPut << "private ";            break;
        case TYPE_ATTRIBUTE_NESTED_FAMILY:        outPut << "protected ";          break;
        case TYPE_ATTRIBUTE_NESTED_FAM_OR_ASSEM:  outPut << "protected internal "; break;
    }

    if      (flags & TYPE_ATTRIBUTE_ABSTRACT && flags & TYPE_ATTRIBUTE_SEALED)
        outPut << "static ";
    else if (!(flags & TYPE_ATTRIBUTE_INTERFACE) && flags & TYPE_ATTRIBUTE_ABSTRACT)
        outPut << "abstract ";
    else if (!is_valuetype && !is_enum && flags & TYPE_ATTRIBUTE_SEALED)
        outPut << "sealed ";

    if      (flags & TYPE_ATTRIBUTE_INTERFACE) outPut << "interface ";
    else if (is_enum)                          outPut << "enum ";
    else if (is_valuetype)                     outPut << "struct ";
    else                                       outPut << "class ";

    outPut << get_type_name(type);

    std::vector<std::string> extends;
    auto parent = il2cpp_class_get_parent(klass);
    if (!is_valuetype && !is_enum && parent) {
        auto parent_type = il2cpp_class_get_type(parent);
        if (parent_type->type != IL2CPP_TYPE_OBJECT)
            extends.emplace_back(get_type_name(parent_type));
    }
    void *iter = nullptr;
    while (auto itf = il2cpp_class_get_interfaces(klass, &iter))
        extends.emplace_back(get_type_name(il2cpp_class_get_type(itf)));

    if (!extends.empty()) {
        outPut << " : " << extends[0];
        for (int i = 1; i < (int)extends.size(); ++i)
            outPut << ", " << extends[i];
    }

    outPut << " // TypeDefIndex: " << std::dec << typeDefIndex;
    outPut << "\n{";
    outPut << dump_field(klass);
    outPut << dump_property(klass);
    outPut << dump_method(klass);
    outPut << "}\n";
    return outPut.str();
}

void il2cpp_api_init(void *handle) {
    LOGI("il2cpp_handle: %p", handle);
    init_il2cpp_api(handle);
    if (il2cpp_domain_get_assemblies) {
        Dl_info dlInfo;
        if (dladdr((void *)il2cpp_domain_get_assemblies, &dlInfo))
            il2cpp_base = reinterpret_cast<uint64_t>(dlInfo.dli_fbase);
        LOGI("il2cpp_base: %" PRIx64"", il2cpp_base);
    } else {
        LOGE("Failed to initialize il2cpp api.");
        return;
    }
    while (!il2cpp_is_vm_thread(nullptr)) {
        LOGI("Waiting for il2cpp_init...");
        sleep(1);
    }
    auto domain = il2cpp_domain_get();
    il2cpp_thread_attach(domain);
}

void il2cpp_dump(const char *outDir) {
    LOGI("dumping...");
    size_t size;
    auto domain     = il2cpp_domain_get();
    auto assemblies = il2cpp_domain_get_assemblies(domain, &size);

    std::stringstream imageOutput;
    for (int i = 0; i < (int)size; ++i) {
        auto image = il2cpp_assembly_get_image(assemblies[i]);
        imageOutput << "// Image " << i << ": " << il2cpp_image_get_name(image) << "\n";
    }

    std::vector<std::string> outPuts;
    int typeDefIndex = 0;

    if (il2cpp_image_get_class) {
        LOGI("Version greater than 2018.3");
        for (int i = 0; i < (int)size; ++i) {
            auto image = il2cpp_assembly_get_image(assemblies[i]);
            std::stringstream imageStr;
            imageStr << "\n// Dll : " << il2cpp_image_get_name(image);
            auto classCount = il2cpp_image_get_class_count(image);
            for (int j = 0; j < (int)classCount; ++j) {
                auto klass  = il2cpp_image_get_class(image, j);
                auto type   = il2cpp_class_get_type(const_cast<Il2CppClass *>(klass));
                outPuts.push_back(imageStr.str() + dump_type(type, typeDefIndex));
                typeDefIndex++;
            }
        }
    } else {
        LOGI("Version less than 2018.3");
        auto corlib           = il2cpp_get_corlib();
        auto assemblyClass    = il2cpp_class_from_name(corlib, "System.Reflection", "Assembly");
        auto assemblyLoad     = il2cpp_class_get_method_from_name(assemblyClass, "Load", 1);
        auto assemblyGetTypes = il2cpp_class_get_method_from_name(assemblyClass, "GetTypes", 0);
        if (!assemblyLoad     || !assemblyLoad->methodPointer)     { LOGI("miss Assembly::Load");     return; }
        if (!assemblyGetTypes || !assemblyGetTypes->methodPointer) { LOGI("miss Assembly::GetTypes"); return; }

        typedef void        *(*Assembly_Load_ftn)(void *, Il2CppString *, void *);
        typedef Il2CppArray *(*Assembly_GetTypes_ftn)(void *, void *);

        for (int i = 0; i < (int)size; ++i) {
            auto image      = il2cpp_assembly_get_image(assemblies[i]);
            std::stringstream imageStr;
            auto image_name = il2cpp_image_get_name(image);
            imageStr << "\n// Dll : " << image_name;
            auto imageName       = std::string(image_name);
            auto pos             = imageName.rfind('.');
            auto imageNameNoExt  = imageName.substr(0, pos);
            auto assemblyFileName   = il2cpp_string_new(imageNameNoExt.data());
            auto reflectionAssembly = ((Assembly_Load_ftn)assemblyLoad->methodPointer)(nullptr, assemblyFileName, nullptr);
            auto reflectionTypes    = ((Assembly_GetTypes_ftn)assemblyGetTypes->methodPointer)(reflectionAssembly, nullptr);
            auto items = reflectionTypes->vector;
            for (int j = 0; j < (int)reflectionTypes->max_length; ++j) {
                auto klass  = il2cpp_class_from_system_type((Il2CppReflectionType *)items[j]);
                auto type   = il2cpp_class_get_type(klass);
                outPuts.push_back(imageStr.str() + dump_type(type, typeDefIndex));
                typeDefIndex++;
            }
        }
    }

    LOGI("write dump file");
    auto outPath = std::string(outDir).append("/files/dump.cs");
    std::ofstream outStream(outPath);
    outStream << imageOutput.str();
    for (auto &s : outPuts) outStream << s;
    outStream.close();
    LOGI("dump done!");
}
