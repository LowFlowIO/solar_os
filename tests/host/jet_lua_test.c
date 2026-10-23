#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "solar_os_jet.h"
#include "solar_os_display.h"
#include "jet_fixture.h"
#define SOLAR_OS_PACKAGE_SERVICE_JET 1
#define SOLUA_EVENT_JET_MODE 1
#define pdMS_TO_TICKS(value) (value)
#define SOLAR_OS_LOGW(...) ((void)0)
typedef struct { int type,attr; } solua_event_t;
typedef struct { int unused; } solar_os_context_t;
static struct {
    solar_os_jet_t *jet;
    uint32_t jet_generation,jet_count;
    bool jet_busy,jet_ack,jet_suspended;
} state,*solua_runtime_state=&state;
static struct { bool stop_requested,tui_active; void *events; solar_os_gfx_t *session_gfx,*claimed_gfx; } solua;
static bool mode;
static unsigned visibility_race;
static bool solua_should_cancel(void *user) {(void)user;return solua.stop_requested;}
static bool solua_emotion_drawing(void) {return false;}
static void vTaskDelay(unsigned delay) {
    if(visibility_race==1 && delay==1) {
        assert(solar_os_jet_suspend(state.jet)==ESP_OK);
        state.jet_suspended=true;state.jet_busy=false;visibility_race=2;
    } else if(visibility_race==2 && delay==10) {
        assert(solar_os_jet_resume(state.jet)==ESP_OK);
        state.jet_suspended=false;visibility_race=0;
    }
}
static void solua_check_esp(lua_State *L,int err) {if(err)luaL_error(L,"service error %d",err);}
static void solua_set_func(lua_State *L,int table,const char *name,lua_CFunction f) {
    table=lua_absindex(L,table);lua_pushcfunction(L,f);lua_setfield(L,table,name);
}
static void solua_set_int(lua_State *L,int table,const char *name,lua_Integer n) {
    table=lua_absindex(L,table);lua_pushinteger(L,n);lua_setfield(L,table,name);
}
static bool solua_send_event(const solua_event_t *event) {
    assert(event->type==SOLUA_EVENT_JET_MODE);mode=event->attr;state.jet_ack=true;return true;
}
#include "solar_os_lua_jet.inc"
static void solua_drain_events(solar_os_context_t *ctx) {(void)ctx;state.jet_busy=false;state.jet_ack=true;}
#include "jet_visibility.inc"
static void run(lua_State *L,const char *source) {
    if(luaL_dostring(L,source)) {fprintf(stderr,"%s\n",lua_tostring(L,-1));abort();}
}
static int should_exit(lua_State *L) {lua_pushboolean(L,false);return 1;}
static int sleep_ms(lua_State *L) {assert(luaL_checkinteger(L,1)>=0);return 0;}
int main(int argc,char **argv) {
    for(unsigned mono=0;mono<2;++mono) {
        jet_test_reset(mono);memset(&state,0,sizeof(state));memset(&solua,0,sizeof(solua));
        lua_State *L=luaL_newstate();assert(L);luaL_openlibs(L);
        lua_newtable(L);solua_set_func(L,-1,"open",solua_jet_open);lua_setglobal(L,"jet");
        run(L,"assert(not pcall(jet.open)); assert(not pcall(jet.open,'missing')); "
            "assert(not pcall(jet.open,'display0',65536)); assert(not pcall(jet.open,'display0',-1)); "
            "assert(not pcall(jet.open,'display0\\0ignored')); "
            "s=jet.open('display0'); assert(not pcall(jet.open,'display0')); "
            "assert(not pcall(s.cube,s,65536,10,10)); assert(not pcall(s.cube,s,10,10,10,-1)); "
            "assert(not pcall(s.camera,s,0,0,-500,0,0,0,1)); "
            "assert(not pcall(s.light,s,0,0,65536)); "
            "s:background(0xffffff); s:camera(); s:light(); "
            "cube=s:cube(150,150,150,0x2192ff); s:transform(cube,-55,0,100,15,30,15); s:render(); "
            "assert(s:status().frames==1 and s:status().presented==1); "
            "assert(not pcall(s.mesh,s,{},{})); "
            "assert(not pcall(s.mesh,s,{{0,0,0},{1,1,1},{2,2,2}},{{0,1,2}})); "
            "assert(not pcall(s.mesh,s,{{0,nil,0},{1,1,1},{2,2,2}},{{1,2,3}})); "
            "m=s:mesh({{-50,-50,0},{50,-50,0},{0,50,0}},{{1,2,3}},0xff0000); "
            "s:remove(m); assert(not pcall(s.transform,s,m)); "
            "s:close();s:close();assert(not pcall(s.render,s)); "
            "g=jet.open('display0');s:close();assert(g:status().width==64);g:close(); "
            "assert(not pcall(function() local a <close> = jet.open('display0');error('script error') end))");
        jet_test_assert_clean();
        run(L,"s=jet.open('display0');assert(s:capabilities().textures and s:capabilities().materials); "
            "t=s:texture(2,2,string.rep(string.char(255,0,0),4),{format='rgb888',name='red'}); "
            "assert(s:texture_info(t).width==2 and s:texture_info(t).format=='rgb888'); "
            "m=s:material{texture=t,shading='unlit',name='surface'}; "
            "assert(s:material_info(m).texture==t and s:material_info(m).shading=='unlit'); "
            "c=s:cube(150,150,150);s:assign(c,m);s:render(); "
            "assert(not pcall(s.remove,s,t));assert(not pcall(s.remove,s,m)); "
            "assert(not pcall(s.material_set,s,m,{shading='typo'})); "
            "assert(not pcall(s.texture_set,s,t,{unknown=true})); "
            "assert(not pcall(s.texture,s,1,1,'\\0',{format='index8',palette={}})); "
            "s:material_set(m,{alpha=180,specular=32});assert(s:material_info(m).alpha==180); "
            "s:texture_set(t,{pixels=string.rep(string.char(0,255,0),4)});s:render(); "
            "i=s:texture(2,2,string.char(0,1,1,0),{format='index8',palette={0xffffff,0x000000}}); "
            "s:texture_set(i,{offset=1,palette={0xff0000,0x00ff00}}); "
            "mesh=s:mesh_data({{-80,-80,0,0,0,0,0,-1},{0,80,0,0.5,1,0,0,-1},{80,-80,0,1,0,0,0,-1}},{{1,2,3,m}}); "
            "s:render();s:remove(mesh);s:assign(c,0);s:remove(m);s:remove(t);s:remove(i);s:remove(c);s:close()");
        jet_test_assert_clean();
        solua.session_gfx=jet_test_gfx();solua.events=(void *)1;
        run(L,"s=jet.open('display0');assert(s:capabilities().geometry and s:capabilities().scene); "
            "for _,kind in ipairs{'cube','grid','plane','pyramid','sphere','capsule','cylinder','billboard','quad','debug_cube'} do "
            "local p=s:primitive{kind=kind};local info=s:object_info(p);assert(info.vertices>0 and info.triangles>0); "
            "s:object_set(p,{x=10,scale_x=0.5,culling='none',blend='average',depth_bias=2,name='shape'}); "
            "assert(s:object_info(p).x==10 and s:object_info(p).scale_x==0.5); "
            "s:object_action(p,{action='bake_rotation'});s:object_action(p,{action='bake_scale',numerator=1,denominator=2}); "
            "s:object_action(p,{action='cache_positions'});local copy=s:object_action(p,{action='clone'}); "
            "s:remove(p);s:render();s:remove(copy);end; "
            "a=s:primitive{kind='sphere',segments=16};b=s:primitive{kind='sphere',segments=6}; "
            "s:object_set(b,{enabled=false});s:lod(a,{b});assert(s:object_info(a).lod_levels==1); "
            "assert(not pcall(s.lod,s,b,{a}));assert(not pcall(s.remove,s,b));s:lod(a,{});s:remove(b); "
            "s:camera_set{z=-450,near=10,far=3000};s:camera_action{action='translate_local',x=2}; "
            "assert(s:camera_info().z==-450 and s:camera_info().near==10); "
            "assert(not pcall(s.camera_set,s,{near=3000,far=10})); "
            "s:settings{lod_scale=200,water_time=2.5};assert(s:settings_info().water_time==2.5); "
            "s:gradient{0xffffff,0x102040};s:render();s:gradient(nil); "
            "p=s:point_light{x=-100,color=0xff0000,range_max=600,name='red'}; "
            "s:point_light_set(p,{enabled=false});assert(not s:point_light_info(p).enabled);s:remove(p); "
            "s:mesh_update(a,{{-60,-60,0},{0,60,0},{60,-60,0}},{{1,2,3}}); "
            "assert(s:object_info(a).triangles==1);s:render();s:close()");
        jet_test_assert_clean();
        run(L,"s=jet.open(); c=s:cube(150,150,150); s:render()");assert(mode&&solua_jet_drawing());
        state.jet_busy=true;solua_jet_visibility(NULL,false);
        assert(state.jet_suspended&&!solua_jet_active());
        solua_jet_visibility(NULL,true);assert(!state.jet_suspended);
        /* The worker passed its wait just before the UI acquired the lock. */
        state.jet_busy=true;visibility_race=1;
        run(L,"s:render()");assert(visibility_race==0&&!solua_jet_active());
        run(L,"s:render();s:close()");assert(!mode);jet_test_assert_clean();
        run(L,"g=jet.open(); g:cube(150,150,150);g:render()");
        solua_jet_destroy();assert(!solua_jet_drawing()&&!solua_jet_active());jet_test_assert_clean();
        run(L,"assert(not pcall(g.render,g))");
        if(argc>=2) {
            lua_newtable(L);lua_getglobal(L,"jet");lua_setfield(L,-2,"jet");
            solua_set_func(L,-1,"should_exit",should_exit);
            lua_newtable(L);solua_set_func(L,-1,"sleep_ms",sleep_ms);lua_setfield(L,-2,"time");
            lua_getglobal(L,"package");lua_getfield(L,-1,"loaded");
            lua_pushvalue(L,-3);lua_setfield(L,-2,"solaros");lua_pop(L,3);
            if(luaL_dofile(L,argv[1])) {fprintf(stderr,"%s\n",lua_tostring(L,-1));abort();}
            jet_test_assert_clean();assert(!mode);
            size_t bytes;const uint8_t *pixels=jet_test_image(&bytes);unsigned nonwhite=0;
            for(size_t i=0;i<bytes;i+=2) nonwhite+=pixels[i]!=0xff||pixels[i+1]!=0xff;
            assert(nonwhite>10&&nonwhite<64*48/2);
            for(int example=2;example<argc;++example) {
                if(luaL_dofile(L,argv[example])) {fprintf(stderr,"%s\n",lua_tostring(L,-1));abort();}
                jet_test_assert_clean();assert(!mode);
            }
        }
        lua_close(L);jet_test_assert_clean();
    }
    puts("Lua Jet objects, mesh validation, stale handles, foreground lifecycle and example passed");return 0;
}
