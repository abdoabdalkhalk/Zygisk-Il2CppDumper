#include "offset_finder.h"
#include "log.h"
#include "il2cpp-class.h"
#include <cstring>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

#define DO_API(r, n, p) extern r (*n) p;
#include "il2cpp-api-functions.h"
#undef DO_API

struct Vec3  { float x, y, z; };
struct Quat4 { float x, y, z, w; };

static bool is_valid_ptr(uint32_t p) {
    return p > 0x10000 && p < 0xFFFF0000 && (p & 1) == 0;
}

static bool read32(uint32_t addr, uint32_t &out) {
    auto *p = (uint32_t *)(uintptr_t)addr;
    out = *p;
    return true;
}

static bool read_float(uint32_t addr, float &out) {
    auto *p = (float *)(uintptr_t)addr;
    out = *p;
    return true;
}

static bool floats_match(float a, float b) {
    if (a == 0.0f && b == 0.0f) return true;
    if (std::abs(a) < 0.0001f && std::abs(b) < 0.0001f) return true;
    return std::abs(a - b) < 0.01f;
}

static bool vec3_match(uint32_t addr, const Vec3 &known) {
    float x, y, z;
    if (!read_float(addr, x)) return false;
    if (!read_float(addr + 4, y)) return false;
    if (!read_float(addr + 8, z)) return false;
    return floats_match(x, known.x) && floats_match(y, known.y) && floats_match(z, known.z);
}

static bool quat_match(uint32_t addr, const Quat4 &known) {
    float x, y, z, w;
    if (!read_float(addr, x)) return false;
    if (!read_float(addr + 4, y)) return false;
    if (!read_float(addr + 8, z)) return false;
    if (!read_float(addr + 12, w)) return false;
    return floats_match(x, known.x) && floats_match(y, known.y) &&
           floats_match(z, known.z) && floats_match(w, known.w);
}

static Il2CppClass* find_class(const char *ns, const char *name) {
    size_t count;
    auto domain = il2cpp_domain_get();
    auto asms   = il2cpp_domain_get_assemblies(domain, &count);
    for (size_t i = 0; i < count; i++) {
        auto img = il2cpp_assembly_get_image(asms[i]);
        auto cls = il2cpp_class_from_name(img, ns, name);
        if (cls) return cls;
    }
    return nullptr;
}

void find_transform_offsets(const char *outDir) {
    LOGI("[OffsetFinder] starting...");

    // ══════════════════════════════════════════
    // Step 1: Wait for game to be ready
    // ══════════════════════════════════════════
    sleep(20); // wait for game scene to load

    auto domain = il2cpp_domain_get();
    il2cpp_thread_attach(domain);

    // ══════════════════════════════════════════
    // Step 2: Find Camera and Transform classes
    // ══════════════════════════════════════════
    auto cameraClass    = find_class("UnityEngine", "Camera");
    auto transformClass = find_class("UnityEngine", "Transform");
    auto objectClass    = find_class("UnityEngine", "Object");

    if (!cameraClass || !transformClass) {
        LOGE("[OffsetFinder] Camera or Transform class not found");
        return;
    }
    LOGI("[OffsetFinder] classes found");

    // ══════════════════════════════════════════
    // Step 3: Wait FOREVER until Camera.main is ready AND in-game
    // ══════════════════════════════════════════
    auto getMain = il2cpp_class_get_method_from_name(cameraClass, "get_main", 0);
    if (!getMain) {
        LOGE("[OffsetFinder] Camera.get_main not found");
        return;
    }

    Il2CppObject *mainCam = nullptr;
    int waitedSeconds = 0;

    while (true) {
        Il2CppException *exc = nullptr;
        mainCam = il2cpp_runtime_invoke(getMain, nullptr, nullptr, &exc);
        
        if (mainCam && !exc) {
            // Camera موجودة — نتحقق أنها فعلياً في game (position != 0)
            auto getPos = il2cpp_class_get_method_from_name(transformClass, "get_position", 0);
            auto getTransformMethod = il2cpp_class_get_method_from_name(cameraClass, "get_transform", 0);
            
            if (getTransformMethod && getPos) {
                exc = nullptr;
                auto tmpTransform = il2cpp_runtime_invoke(getTransformMethod, mainCam, nullptr, &exc);
                
                if (tmpTransform && !exc) {
                    exc = nullptr;
                    auto tmpPosBoxed = il2cpp_runtime_invoke(getPos, tmpTransform, nullptr, &exc);
                    
                    if (tmpPosBoxed && !exc) {
                        Vec3 tmpPos = *(Vec3 *)il2cpp_object_unbox(tmpPosBoxed);
                        
                        // شرط: position مش (0,0,0) يعني فعلياً في game
                        if (std::abs(tmpPos.x) > 0.5f || 
                            std::abs(tmpPos.y) > 0.5f || 
                            std::abs(tmpPos.z) > 0.5f)
                        {
                            LOGI("[OffsetFinder] Ready! Camera at (%.2f, %.2f, %.2f) after %d sec",
                                 tmpPos.x, tmpPos.y, tmpPos.z, waitedSeconds);
                            break; // خرج من اللوب — جاهزين
                        }
                    }
                }
            }
        }
        
        // log كل 30 ثانية
        if (waitedSeconds % 30 == 0) {
            LOGI("[OffsetFinder] still waiting for game... (%d sec elapsed)", waitedSeconds);
        }
        
        sleep(2);
        waitedSeconds += 2;
    }
    
    // ══════════════════════════════════════════
    // Step 4: Get Camera.main.transform
    // ══════════════════════════════════════════
    const MethodInfo *getTransform = nullptr;
    auto cls = cameraClass;
    while (cls && !getTransform) {
        getTransform = il2cpp_class_get_method_from_name(cls, "get_transform", 0);
        cls = il2cpp_class_get_parent(cls);
    }

    if (!getTransform) {
        LOGE("[OffsetFinder] get_transform not found");
        return;
    }

    Il2CppException *exc = nullptr;
    auto transformObj = il2cpp_runtime_invoke(getTransform, mainCam, nullptr, &exc);
    if (!transformObj || exc) {
        LOGE("[OffsetFinder] transform is null");
        return;
    }
    LOGI("[OffsetFinder] transform = %p", transformObj);

    // ══════════════════════════════════════════
    // Step 5: Call get_position() to get known position
    // ══════════════════════════════════════════
    auto getPos = il2cpp_class_get_method_from_name(transformClass, "get_position", 0);
    auto getRot = il2cpp_class_get_method_from_name(transformClass, "get_rotation", 0);
    auto getScl = il2cpp_class_get_method_from_name(transformClass, "get_localScale", 0);

    if (!getPos || !getRot) {
        LOGE("[OffsetFinder] get_position or get_rotation not found");
        return;
    }

    exc = nullptr;
    auto posBoxed = il2cpp_runtime_invoke(getPos, transformObj, nullptr, &exc);
    Vec3 knownPos = *(Vec3 *)il2cpp_object_unbox(posBoxed);

    // تأكد أن الـ position ليست (0,0,0) - يعني الكاميرا فعلياً في مكان
    if (std::abs(knownPos.x) < 0.1f && 
        std::abs(knownPos.y) < 0.1f && 
        std::abs(knownPos.z) < 0.1f)
    {
        LOGE("[OffsetFinder] Camera position is (0,0,0) - not in game yet");
        return;
    }

    LOGI("[OffsetFinder] position = (%.2f, %.2f, %.2f)", knownPos.x, knownPos.y, knownPos.z);

    exc = nullptr;
    auto rotBoxed = il2cpp_runtime_invoke(getRot, transformObj, nullptr, &exc);
    Quat4 knownRot = *(Quat4 *)il2cpp_object_unbox(rotBoxed);
    LOGI("[OffsetFinder] rotation = (%.4f, %.4f, %.4f, %.4f)", knownRot.x, knownRot.y, knownRot.z, knownRot.w);

    Vec3 knownScale = {1, 1, 1};
    if (getScl) {
        exc = nullptr;
        auto sclBoxed = il2cpp_runtime_invoke(getScl, transformObj, nullptr, &exc);
        if (sclBoxed) knownScale = *(Vec3 *)il2cpp_object_unbox(sclBoxed);
    }
    LOGI("[OffsetFinder] scale = (%.2f, %.2f, %.2f)", knownScale.x, knownScale.y, knownScale.z);

    // ══════════════════════════════════════════
    // Step 6: Find m_CachedPtr offset
    // ══════════════════════════════════════════
    uint32_t managedAddr = (uint32_t)(uintptr_t)transformObj;
    uint32_t cachedPtrOffset = 0;
    uint32_t nativePtr = 0;

    // On 32-bit IL2CPP: header = 8 bytes (klass + monitor)
    // m_CachedPtr is usually at +0x8 (first field of UnityEngine.Object)
    if (objectClass) {
        auto cachedField = il2cpp_class_get_field_from_name(objectClass, "m_CachedPtr");
        if (cachedField) {
            cachedPtrOffset = (uint32_t)il2cpp_field_get_offset(cachedField);
            LOGI("[OffsetFinder] m_CachedPtr offset from API = 0x%X", cachedPtrOffset);
        }
    }

    if (cachedPtrOffset == 0) {
        // fallback: scan for valid pointer
        for (uint32_t off = 0x8; off <= 0x20; off += 4) {
            uint32_t val;
            if (read32(managedAddr + off, val) && is_valid_ptr(val)) {
                cachedPtrOffset = off;
                break;
            }
        }
    }

    if (cachedPtrOffset == 0) {
        LOGE("[OffsetFinder] m_CachedPtr not found");
        return;
    }

    read32(managedAddr + cachedPtrOffset, nativePtr);
    LOGI("[OffsetFinder] m_CachedPtr offset = 0x%X, nativePtr = 0x%X", cachedPtrOffset, nativePtr);

    if (!is_valid_ptr(nativePtr)) {
        LOGE("[OffsetFinder] nativePtr invalid");
        return;
    }

    // ══════════════════════════════════════════
    // Step 7: Brute-force scan for position/rotation in native memory
    // Chain: nativePtr + off1 → ptr1 + off2 → ptr2 + off3 → floats
    // ══════════════════════════════════════════
    std::string outPath = std::string(outDir) + "/files/transform_offsets.txt";
    std::ofstream out(outPath);

    out << "// ═══════════════════════════════════════════════════════════\n";
    out << "// Transform Offset Finder Results\n";
    out << "// ═══════════════════════════════════════════════════════════\n";
    out << "// Managed Transform addr: 0x" << std::hex << managedAddr << "\n";
    out << "// m_CachedPtr offset: 0x" << cachedPtrOffset << "\n";
    out << "// Native Transform ptr: 0x" << nativePtr << "\n";
    out << "// Known position: (" << knownPos.x << ", " << knownPos.y << ", " << knownPos.z << ")\n";
    out << "// Known rotation: (" << knownRot.x << ", " << knownRot.y << ", " << knownRot.z << ", " << knownRot.w << ")\n";
    out << "// Known scale: (" << knownScale.x << ", " << knownScale.y << ", " << knownScale.z << ")\n";
    out << "// ═══════════════════════════════════════════════════════════\n\n";
    out << std::dec;

    LOGI("[OffsetFinder] scanning native memory...");

    struct FoundOffset {
        uint32_t off1, off2, off3;
        std::string type; // "position" / "rotation" / "scale"
    };
    std::vector<FoundOffset> found;

    // Scan: nativePtr + off1 → ptr1
    for (uint32_t off1 = 0x0; off1 <= 0x80; off1 += 4) {
        uint32_t ptr1;
        if (!read32(nativePtr + off1, ptr1) || !is_valid_ptr(ptr1)) continue;

        // ptr1 + off2 → ptr2
        for (uint32_t off2 = 0x0; off2 <= 0x80; off2 += 4) {
            uint32_t ptr2;
            if (!read32(ptr1 + off2, ptr2) || !is_valid_ptr(ptr2)) continue;

            // ptr2 + off3 → float data
            for (uint32_t off3 = 0x0; off3 <= 0x200; off3 += 4) {
                // check position
                if (vec3_match(ptr2 + off3, knownPos)) {
                    found.push_back({off1, off2, off3, "POSITION"});
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                        "[POSITION] nativePtr+0x%X → +0x%X → +0x%X  (chain: 0x%X → 0x%X → 0x%X)\n",
                        off1, off2, off3, nativePtr + off1, ptr1 + off2, ptr2 + off3);
                    out << buf;
                    LOGI("[OffsetFinder] %s", buf);
                }

                // check rotation
                if (quat_match(ptr2 + off3, knownRot)) {
                    found.push_back({off1, off2, off3, "ROTATION"});
                    char buf[256];
                    snprintf(buf, sizeof(buf),
                        "[ROTATION] nativePtr+0x%X → +0x%X → +0x%X  (chain: 0x%X → 0x%X → 0x%X)\n",
                        off1, off2, off3, nativePtr + off1, ptr1 + off2, ptr2 + off3);
                    out << buf;
                    LOGI("[OffsetFinder] %s", buf);
                }

                // check scale
                if (vec3_match(ptr2 + off3, knownScale) && knownScale.x != 0) {
                    // scale (1,1,1) might match many places, only log if near position
                    bool nearPosition = false;
                    for (auto &f : found) {
                        if (f.type == "POSITION" && f.off1 == off1 && f.off2 == off2) {
                            int diff = (int)off3 - (int)f.off3;
                            if (diff > 0 && diff <= 0x40) nearPosition = true;
                        }
                    }
                    if (nearPosition) {
                        found.push_back({off1, off2, off3, "SCALE"});
                        char buf[256];
                        snprintf(buf, sizeof(buf),
                            "[SCALE]    nativePtr+0x%X → +0x%X → +0x%X  (chain: 0x%X → 0x%X → 0x%X)\n",
                            off1, off2, off3, nativePtr + off1, ptr1 + off2, ptr2 + off3);
                        out << buf;
                        LOGI("[OffsetFinder] %s", buf);
                    }
                }
            }
        }
    }

    // ══════════════════════════════════════════
    // Step 8: Also scan 2-level deep (off1 → off2 → floats directly)
    // In case the chain is shorter
    // ══════════════════════════════════════════
    out << "\n// ═══ 2-level scan (nativePtr + off1 → floats) ═══\n\n";

    for (uint32_t off1 = 0x0; off1 <= 0x200; off1 += 4) {
        if (vec3_match(nativePtr + off1, knownPos)) {
            char buf[256];
            snprintf(buf, sizeof(buf), "[POSITION-DIRECT] nativePtr+0x%X\n", off1);
            out << buf;
            LOGI("[OffsetFinder] %s", buf);
        }
        if (quat_match(nativePtr + off1, knownRot)) {
            char buf[256];
            snprintf(buf, sizeof(buf), "[ROTATION-DIRECT] nativePtr+0x%X\n", off1);
            out << buf;
            LOGI("[OffsetFinder] %s", buf);
        }
    }

    // 2-level: nativePtr + off1 → ptr1 + off2 → floats
    out << "\n// ═══ 2-level scan (nativePtr + off1 → ptr + off2 → floats) ═══\n\n";

    for (uint32_t off1 = 0x0; off1 <= 0x80; off1 += 4) {
        uint32_t ptr1;
        if (!read32(nativePtr + off1, ptr1) || !is_valid_ptr(ptr1)) continue;

        for (uint32_t off2 = 0x0; off2 <= 0x200; off2 += 4) {
            if (vec3_match(ptr1 + off2, knownPos)) {
                char buf[256];
                snprintf(buf, sizeof(buf), "[POSITION-2LVL] nativePtr+0x%X → +0x%X\n", off1, off2);
                out << buf;
                LOGI("[OffsetFinder] %s", buf);
            }
            if (quat_match(ptr1 + off2, knownRot)) {
                char buf[256];
                snprintf(buf, sizeof(buf), "[ROTATION-2LVL] nativePtr+0x%X → +0x%X\n", off1, off2);
                out << buf;
                LOGI("[OffsetFinder] %s", buf);
            }
        }
    }

    // ══════════════════════════════════════════
    // Summary
    // ══════════════════════════════════════════
    out << "\n// ═══════════════════════════════════════════════════════════\n";
    out << "// SUMMARY\n";
    out << "// ═══════════════════════════════════════════════════════════\n";
    out << "// Total results found: " << found.size() << "\n";
    out << "//\n";
    out << "// Usage in code:\n";
    out << "//   ReadZ(managedTransform + 0x" << std::hex << cachedPtrOffset << ", nativePtr);  // m_CachedPtr\n";

    for (auto &f : found) {
        if (f.type == "POSITION") {
            out << "//   ReadZ(nativePtr + 0x" << std::hex << f.off1 << ", ptr1);\n";
            out << "//   ReadZ(ptr1 + 0x" << f.off2 << ", ptr2);\n";
            out << "//   ReadZ(ptr2 + 0x" << f.off3 << ", position);  // Vector3\n";
        }
        if (f.type == "ROTATION") {
            out << "//   ReadZ(ptr2 + 0x" << f.off3 << ", rotation);  // Quaternion\n";
        }
        if (f.type == "SCALE") {
            out << "//   ReadZ(ptr2 + 0x" << f.off3 << ", scale);     // Vector3\n";
        }
    }
    out << "// ═══════════════════════════════════════════════════════════\n";

    out.close();
    LOGI("[OffsetFinder] done! results saved to %s", outPath.c_str());
    LOGI("[OffsetFinder] found %zu offset chains", found.size());
}
