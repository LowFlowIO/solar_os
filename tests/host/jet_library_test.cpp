#include "solar_os_jet_abi.h"
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <new>
#include <vector>
extern "C" const solar_os_jet_api_v1_t *solar_os_jet_engine_api_v1(void);
extern "C" bool solar_os_jet_engine_idle(void);
static long requests,fail_at=-1,live;
void *operator new(size_t bytes) {
    if(requests++==fail_at) throw std::bad_alloc();
    void *p=std::malloc(bytes?bytes:1);if(!p)throw std::bad_alloc();++live;return p;
}
void operator delete(void *p) noexcept {if(p){--live;std::free(p);}}
void operator delete(void *p,size_t) noexcept {operator delete(p);}
void *operator new[](size_t n) {return operator new(n);}
void operator delete[](void *p) noexcept {operator delete(p);}
void operator delete[](void *p,size_t) noexcept {operator delete(p);}
static bool cancellation(void *user) {return ++*static_cast<unsigned *>(user)==4;}
static int surface_chain(const solar_os_jet_api_v1_t *api,void *scene,uint16_t *color,size_t bytes) {
    const uint8_t pixels[]={255,0,0,0,255,0,0,0,255,255,255,0};
    solar_os_jet_command_t q={};solar_os_jet_reply_t r;
    q.opcode=SOLAR_OS_JET_TEXTURE;q.values[16]=q.values[17]=2;q.data=pixels;q.bytes=sizeof(pixels);
    q.fields=1U<<JET_TEX_format;q.values[JET_TEX_format]=1;
    int err=api->extensions->execute(scene,&q,&r);if(err)return err;uint32_t tex=r.handle;
    q={};q.opcode=SOLAR_OS_JET_TEXTURE_SET;q.handle=tex;q.fields=1U<<JET_TEX_offset;q.values[JET_TEX_offset]=1;
    long before=requests;err=api->extensions->execute(scene,&q,&r);
    assert(!err&&requests==before); // Palette/sampling animation must not copy or allocate pixels.
    q={};q.opcode=SOLAR_OS_JET_MATERIAL;q.fields=(1U<<JET_MAT_texture)|(1U<<JET_MAT_shading);
    q.values[JET_MAT_texture]=tex;q.values[JET_MAT_shading]=4;
    err=api->extensions->execute(scene,&q,&r);if(err)return err;uint32_t mat=r.handle,cube=0;
    err=api->cube(scene,150,150,150,0xffffff,&cube);if(err)return err;
    q={};q.opcode=SOLAR_OS_JET_ASSIGN;q.handle=cube;q.values[0]=mat;
    err=api->extensions->execute(scene,&q,&r);if(err)return err;
    err=api->render(scene,color,bytes,nullptr,nullptr);if(err)return err;
    assert(api->remove(scene,tex)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(api->remove(scene,mat)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    // Updating a live texture preserves its address and all material references.
    q={};q.opcode=SOLAR_OS_JET_TEXTURE_SET;q.handle=tex;q.data=pixels;q.bytes=sizeof(pixels);
    err=api->extensions->execute(scene,&q,&r);if(err)return err;
    for(unsigned shading=0;shading<7;++shading) {
        q={};q.opcode=SOLAR_OS_JET_MATERIAL_SET;q.handle=mat;q.fields=1U<<JET_MAT_shading;q.values[JET_MAT_shading]=shading;
        err=api->extensions->execute(scene,&q,&r);if(err)return err;
        err=api->render(scene,color,bytes,nullptr,nullptr);if(err)return err;
    }
    assert(api->remove(scene,cube)==0&&api->remove(scene,mat)==0&&api->remove(scene,tex)==0);
    return 0;
}
int main() {
    auto *api=solar_os_jet_engine_api_v1();assert(api->abi_version==1&&api->struct_size==sizeof(*api));
    std::vector<uint16_t> color(64*48),depth(64*48);size_t bytes=color.size()*2;
    long baseline=live;bool complete=false;
    for(long failure=0;failure<150;++failure) {
        requests=0;fail_at=failure;void *scene=nullptr;
        int err=api->create(64,48,color.data(),depth.data(),bytes,&scene);
        if(!err) {
            uint32_t object=0;err=api->cube(scene,150,150,150,0x2192ff,&object);
            if(!err) err=api->render(scene,color.data(),bytes,nullptr,nullptr);
            assert(api->destroy(scene)==0);
        }
        assert(err==0||err==SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
        assert(solar_os_jet_engine_idle()&&live==baseline);
        if(requests<=failure){complete=true;break;}
    }
    assert(complete);fail_at=-1;void *scene=nullptr,*second=nullptr;
    complete=false;
    for(long failure=0;failure<250;++failure) {
        requests=0;fail_at=failure;void *s=nullptr;
        int err=api->create(64,48,color.data(),depth.data(),bytes,&s);
        if(!err) {err=surface_chain(api,s,color.data(),bytes);assert(api->destroy(s)==0);}
        assert(err==0||err==SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
        assert(solar_os_jet_engine_idle()&&live==baseline);
        if(requests<=failure){complete=true;break;}
    }
    assert(complete);fail_at=-1;
    assert(api->create(63,48,color.data(),depth.data(),bytes,&scene)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->create(64,48,color.data(),depth.data(),bytes,&scene)==0);
    assert(api->create(64,48,color.data(),depth.data(),bytes,&second)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE&&!second);
    uint32_t cube=0;assert(api->cube(scene,150,150,150,0x2192ff,&cube)==0);
    solar_os_jet_pose_t pose={-55,0,100,15,30,15};assert(api->transform(scene,cube,&pose)==0);
    assert(api->render(scene,color.data(),bytes,nullptr,nullptr)==0);
    auto expected=color;unsigned callbacks=0;
    assert(api->render(scene,color.data(),bytes,cancellation,&callbacks)==SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(api->render(scene,color.data(),bytes,nullptr,nullptr)==0&&color==expected);
    solar_os_jet_vertex_t vertices[]={{-50,-50,0},{50,-50,0},{0,50,0}};
    solar_os_jet_triangle_t triangle[]={{0,1,3}};uint32_t mesh=0;
    assert(api->mesh(scene,vertices,3,triangle,1,0xff0000,&mesh)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    triangle[0].c=2;assert(api->mesh(scene,vertices,3,triangle,1,0xff0000,&mesh)==0);
    assert(api->remove(scene,mesh)==0&&api->remove(scene,mesh)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(api->transform(scene,mesh,&pose)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(api->background(scene,0)==0&&api->background(scene,0x1000000)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->remove(scene,cube)==0);
    assert(api->render(scene,color.data(),bytes,nullptr,nullptr)==0);
    for(auto pixel:color)assert(pixel==0);
    assert(api->destroy(scene)==0&&api->destroy(scene)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(solar_os_jet_engine_idle());puts("Jet provider ABI, allocation failures, geometry and cancellation passed");
}
