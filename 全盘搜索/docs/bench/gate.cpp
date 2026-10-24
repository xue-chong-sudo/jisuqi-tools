#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#pragma pack(push,1)
struct Rec { uint32_t nameOff,nameLen,lowerOff,lowerLen,parent; uint64_t size,mtime; uint32_t attrs; uint8_t cat; };
#pragma pack(pop)
int main(){
    printf("sizeof(Rec)=%zu alignof(Rec)=%zu\n", sizeof(Rec), alignof(Rec));
    printf("is_implicit_lifetime? (C++23 trait) under test\n");
#if defined(__cpp_lib_start_lifetime_as) && __cpp_lib_start_lifetime_as >= 202207L
    printf("__cpp_lib_start_lifetime_as = %ld  -> AVAILABLE\n", (long)__cpp_lib_start_lifetime_as);
    alignas(Rec) static unsigned char buf[sizeof(Rec)*4];
    Rec* p = std::start_lifetime_as_array<Rec>(buf, 4);
    printf("start_lifetime_as_array OK, p=%p\n", (void*)p);
#else
    printf("__cpp_lib_start_lifetime_as NOT defined -> std::start_lifetime_as_array 不可用\n");
#endif
    return 0;
}
