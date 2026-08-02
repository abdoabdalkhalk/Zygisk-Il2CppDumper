
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

static const char* primitive_type_name(Il2CppTypeEnum type) {
    switch (type) {
        case IL2CPP_TYPE_VOID:      return "void";
        case IL2CPP_TYPE_BOOLEAN:   return "bool";
        case IL2CPP_TYPE_CHAR:      return "char";
        case IL2CPP_TYPE_I1:        return "sbyte";
        case IL2CPP_TYPE_U1:        return "byte";
        case IL2CPP_TYPE_I2:        return "short";
        case IL2CPP_TYPE_U2:        return "ushort";
        case IL2CPP_TYPE_I4:        return "int";
        case IL2CPP_TYPE_U4:        return "uint";
        case IL2CPP_TYPE_I8:        return "long";
        case IL2CPP_TYPE_U8:        return "ulong";
        case IL2CPP_TYPE_R4:        return "float";
        case IL2CPP_TYPE_R8:        return "double";
        case IL2CPP_TYPE_STRING:    return "string";
        case IL2CPP_TYPE_I:         return "IntPtr";
        case IL2CPP_TYPE_U:         return "UIntPtr";
        case IL2CPP_TYPE_OBJECT:    return "object";
        case IL2CPP_TYPE_TYPEDBYREF:return "TypedReference";
        default:                    return nullptr;
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
    return name;
}

static std::string get_generic_args(Il2CppClass *klass) {
    return "";
}

static std::string get_type_name(const Il2CppType *type) {
    if (!type) return "?";

    const char *prim = primitive_type_name(type->type);
    if (prim) return prim;

    
    Il2CppClass *klass = nullptr;

    switch ((int)type->type) {
        case IL2CPP_TYPE_SZARRAY: {
            // T[]
            if (!il2cpp_class_from_type) break;
            klass = il2cpp_class_from_type(type);
            if (!klass) break;
            Il2CppClass *elem = il2cpp_class_get_element_class(klass);
            if (!elem) break;
            const Il2CppType *elem_type = il2cpp_class_get_type(elem);
            return get_type_name(elem_type) + "[]";
        }

        case IL2CPP_TYPE_ARRAY: {
            // T[,] multi-dim
            if (!il2cpp_class_from_type) break;
            klass = il2cpp_class_from_type(type);
            if (!klass) break;
            Il2CppClass *elem = il2cpp_class_get_element_class(klass);
            if (!elem) break;
            const Il2CppType *elem_type = il2cpp_class_get_type(elem);
            int rank = il2cpp_class_get_rank(klass);
            std::string result = get_type_name(elem_type) + "[";
            for (int i = 1; i < rank; i++) result += ",";
            result += "]";
            return result;
        }

        case IL2CPP_TYPE_PTR: {
            // T*
            if (!il2cpp_class_from_type) break;
            klass = il2cpp_class_from_type(type);
            if (!klass) break;
            Il2CppClass *elem = il2cpp_class_get_element_class(klass);
            if (!elem) break;
            const Il2CppType *elem_type = il2cpp_class_get_type(elem);
            return get_type_name(elem_type) + "*";
        }

        case IL2CPP_TYPE_GENERICINST: {
            if (il2cpp_type_get_name) {
                char *full = il2cpp_type_get_name(type);
                if (full) {
                    std::string s(full);
                    il2cpp_free(full);

                    auto tick_pos = s.find('`');
                    std::string class_part;
                    if (tick_pos != std::string::npos) {
                        auto dot_pos = s.rfind('.', tick_pos);
                        if (dot_pos != std::string::npos)
                            class_part = s.substr(dot_pos + 1, tick_pos - dot_pos - 1);
                        else
                            class_part = s.substr(0, tick_pos);
                    } else {
                        auto dot_pos = s.rfind('.');
                        if (dot_pos != std::string::npos)
                            class_part = s.substr(dot_pos + 1);
                        else
                            class_part = s;
                    }

                    auto open = s.find('[');
                    auto close = s.rfind(']');
                    if (open != std::string::npos && close != std::string::npos && close > open) {
                        std::string args_raw = s.substr(open + 1, close - open - 1);
                        
                        std::string result = class_part + "<";
                        
                        std::vector<std::string> args;
                        int depth = 0;
                        std::string cur;
                        for (char c : args_raw) {
                            if (c == '[') { depth++; cur += c; }
                            else if (c == ']') { depth--; cur += c; }
                            else if (c == ',' && depth == 0) {
                                // trim spaces
                                while (!cur.empty() && cur[0] == ' ') cur = cur.substr(1);
                                args.push_back(cur);
                                cur.clear();
                            } else {
                                cur += c;
                            }
                        }
                        if (!cur.empty()) {
                            while (!cur.empty() && cur[0] == ' ') cur = cur.substr(1);
                            args.push_back(cur);
                        }

                        static const struct { const char *dotnet; const char *csharp; } dotnet_to_csharp[] = {
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

                        for (int i = 0; i < (int)args.size(); i++) {
                            if (i > 0) result += ", ";
                            std::string &arg = args[i];
                            
                            std::string type_name_only;
                            if (!arg.empty() && arg[0] == '[') {
                                auto comma = arg.find(',');
                                if (comma != std::string::npos)
                                    type_name_only = arg.substr(1, comma - 1);
                                else
                                    type_name_only = arg.substr(1, arg.size() - 2);
                            } else {
                                type_name_only = arg;
                            }

                            while (!type_name_only.empty() && type_name_only[0] == ' ')
                                type_name_only = type_name_only.substr(1);

                            bool mapped = false;
                            for (int j = 0; dotnet_to_csharp[j].dotnet; j++) {
                                if (type_name_only == dotnet_to_csharp[j].dotnet) {
                                    result += dotnet_to_csharp[j].csharp;
                                    mapped = true;
                                    break;
                                }
                            }
                            if (!mapped) {
                                auto last_dot = type_name_only.rfind('.');
                                if (last_dot != std::string::npos)
                                    result += type_name_only.substr(last_dot + 1);
                                else
                                    result += type_name_only;
                            }
                        }
                        result += ">";
                        return result;
                    }
                    return class_part;
                }
            }

            klass = il2cpp_class_from_type(type);
            if (!klass) return "?";
            std::string result = get_class_name_no_tick(klass);
            
            return result;
        }

        case IL2CPP_TYPE_VAR:
        case IL2CPP_TYPE_MVAR: {
            klass = il2cpp_class_from_type(type);
            if (!klass) return "T";
            return il2cpp_class_get_name(klass) ? il2cpp_class_get_name(klass) : "T";
        }

        default:
            break;
    }

    if (!il2cpp_class_from_type) return "?";
    klass = il2cpp_class_from_type(type);
    if (!klass) return "?";
    
    const char *name = il2cpp_class_get_name(klass);
    if (!name) return "?";

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

    return name;
}


std::string get_method_modifier(uint32_t flags) {
    std::stringstream outPut;
    auto access = flags & METHOD_ATTRIBUTE_MEMBER_ACCESS_MASK;
    switch (access) {
        case METHOD_ATTRIBUTE_PRIVATE:          outPut << "private ";           break;
        case METHOD_ATTRIBUTE_PUBLIC:           outPut << "public ";            break;
        case METHOD_ATTRIBUTE_FAMILY:           outPut << "protected ";         break;
        case METHOD_ATTRIBUTE_ASSEM:
        case METHOD_ATTRIBUTE_FAM_AND_ASSEM:    outPut << "internal ";          break;
        case METHOD_ATTRIBUTE_FAM_OR_ASSEM:     outPut << "protected internal ";break;
    }
    if (flags & METHOD_ATTRIBUTE_STATIC)    outPut << "static ";
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
    if (il2cpp_type_is_byref) {
        byref = il2cpp_type_is_byref(type);
    }
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
        auto prop = const_cast<PropertyInfo *>(prop_const);
        auto get  = il2cpp_property_get_get_method(prop);
        auto set  = il2cpp_property_get_set_method(prop);
        auto prop_name = il2cpp_property_get_name(prop);
        outPut << "\t";
        Il2CppClass *prop_class = nullptr;
        uint32_t iflags = 0;
        if (get) {
            outPut << get_method_modifier(il2cpp_method_get_flags(get, &iflags));
            auto ret = il2cpp_method_get_return_type(get);
            outPut << get_type_name(ret) << " " << prop_name << " { ";
        } else if (set) {
            outPut << get_method_modifier(il2cpp_method_get_flags(set, &iflags));
            auto param = il2cpp_method_get_param(set, 0);
            outPut << get_type_name(param) << " " << prop_name << " { ";
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
    void *iter = nullptr;
    while (auto field = il2cpp_class_get_fields(klass, &iter)) {
        outPut << "\t";
        auto attrs = il2cpp_field_get_flags(field);
        auto access = attrs & FIELD_ATTRIBUTE_FIELD_ACCESS_MASK;
        switch (access) {
            case FIELD_ATTRIBUTE_PRIVATE:           outPut << "private ";           break;
            case FIELD_ATTRIBUTE_PUBLIC:            outPut << "public ";            break;
            case FIELD_ATTRIBUTE_FAMILY:            outPut << "protected ";         break;
            case FIELD_ATTRIBUTE_ASSEMBLY:
            case FIELD_ATTRIBUTE_FAM_AND_ASSEM:     outPut << "internal ";          break;
            case FIELD_ATTRIBUTE_FAM_OR_ASSEM:      outPut << "protected internal ";break;
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
    auto *klass = il2cpp_class_from_type(type);
    outPut << "\n// Namespace: " << il2cpp_class_get_namespace(klass) << "\n";
    auto flags = il2cpp_class_get_flags(klass);
    if (flags & TYPE_ATTRIBUTE_SERIALIZABLE) outPut << "[Serializable]\n";

    auto is_valuetype = il2cpp_class_is_valuetype(klass);
    auto is_enum      = il2cpp_class_is_enum(klass);
    auto visibility   = flags & TYPE_ATTRIBUTE_VISIBILITY_MASK;
    switch (visibility) {
        case TYPE_ATTRIBUTE_PUBLIC:
        case TYPE_ATTRIBUTE_NESTED_PUBLIC:      outPut << "public ";            break;
        case TYPE_ATTRIBUTE_NOT_PUBLIC:
        case TYPE_ATTRIBUTE_NESTED_FAM_AND_ASSEM:
        case TYPE_ATTRIBUTE_NESTED_ASSEMBLY:    outPut << "internal ";          break;
        case TYPE_ATTRIBUTE_NESTED_PRIVATE:     outPut << "private ";           break;
        case TYPE_ATTRIBUTE_NESTED_FAMILY:      outPut << "protected ";         break;
        case TYPE_ATTRIBUTE_NESTED_FAM_OR_ASSEM:outPut << "protected internal ";break;
    }
    if (flags & TYPE_ATTRIBUTE_ABSTRACT && flags & TYPE_ATTRIBUTE_SEALED)
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
        if (parent_type->type != IL2CPP_TYPE_OBJECT) {
            extends.emplace_back(get_type_name(parent_type));
        }
    }
    void *iter = nullptr;
    while (auto itf = il2cpp_class_get_interfaces(klass, &iter)) {
        auto itf_type = il2cpp_class_get_type(itf);
        extends.emplace_back(get_type_name(itf_type));
    }
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
        if (dladdr((void *)il2cpp_domain_get_assemblies, &dlInfo)) {
            il2cpp_base = reinterpret_cast<uint64_t>(dlInfo.dli_fbase);
        }
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
    auto domain = il2cpp_domain_get();
    auto assemblies = il2cpp_domain_get_assemblies(domain, &size);
    std::stringstream imageOutput;
    for (int i = 0; i < (int)size; ++i) {
        auto image = il2cpp_assembly_get_image(assemblies[i]);
        imageOutput << "// Image " << i << ": " << il2cpp_image_get_name(image) << "\n";
    }
    std::vector<std::string> outPuts;
    int typeDefIndex = 0; // عدّاد global عبر كل الـ images

    if (il2cpp_image_get_class) {
        LOGI("Version greater than 2018.3");
        for (int i = 0; i < (int)size; ++i) {
            auto image = il2cpp_assembly_get_image(assemblies[i]);
            std::stringstream imageStr;
            imageStr << "\n// Dll : " << il2cpp_image_get_name(image);
            auto classCount = il2cpp_image_get_class_count(image);
            for (int j = 0; j < (int)classCount; ++j) {
                auto klass = il2cpp_image_get_class(image, j);
                auto type  = il2cpp_class_get_type(const_cast<Il2CppClass *>(klass));
                auto outPut = imageStr.str() + dump_type(type, typeDefIndex);
                outPuts.push_back(outPut);
                typeDefIndex++;
            }
        }
    } else {
        LOGI("Version less than 2018.3");
        auto corlib = il2cpp_get_corlib();
        auto assemblyClass   = il2cpp_class_from_name(corlib, "System.Reflection", "Assembly");
        auto assemblyLoad    = il2cpp_class_get_method_from_name(assemblyClass, "Load", 1);
        auto assemblyGetTypes= il2cpp_class_get_method_from_name(assemblyClass, "GetTypes", 0);
        if (!assemblyLoad || !assemblyLoad->methodPointer)    { LOGI("miss Assembly::Load");     return; }
        if (!assemblyGetTypes || !assemblyGetTypes->methodPointer) { LOGI("miss Assembly::GetTypes"); return; }

        typedef void *(*Assembly_Load_ftn)(void *, Il2CppString *, void *);
        typedef Il2CppArray *(*Assembly_GetTypes_ftn)(void *, void *);

        for (int i = 0; i < (int)size; ++i) {
            auto image = il2cpp_assembly_get_image(assemblies[i]);
            std::stringstream imageStr;
            auto image_name = il2cpp_image_get_name(image);
            imageStr << "\n// Dll : " << image_name;
            auto imageName = std::string(image_name);
            auto pos = imageName.rfind('.');
            auto imageNameNoExt = imageName.substr(0, pos);
            auto assemblyFileName = il2cpp_string_new(imageNameNoExt.data());
            auto reflectionAssembly = ((Assembly_Load_ftn)assemblyLoad->methodPointer)(nullptr, assemblyFileName, nullptr);
            auto reflectionTypes    = ((Assembly_GetTypes_ftn)assemblyGetTypes->methodPointer)(reflectionAssembly, nullptr);
            auto items = reflectionTypes->vector;
            for (int j = 0; j < (int)reflectionTypes->max_length; ++j) {
                auto klass = il2cpp_class_from_system_type((Il2CppReflectionType *)items[j]);
                auto type  = il2cpp_class_get_type(klass);
                auto outPut = imageStr.str() + dump_type(type, typeDefIndex);
                outPuts.push_back(outPut);
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
