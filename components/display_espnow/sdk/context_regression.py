"""Compile the pinned SDK's actual context allocation and key APIs with host stubs.
This exercises software context ownership; it makes no claim about radio/crypto.
"""
import pathlib
import subprocess
import sys
import tempfile
root = pathlib.Path(sys.argv[1])
source = (root / 'src/espnow/src/espnow.c').read_text()
def function(name, text=None):
    global source
    if text is not None:
        saved, source = source, text
    start = source.index('esp_err_t ' + name + '(')
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    result = source[start:end]
    if text is not None: source = saved
    return result
start = source.index('    if (config->sec_enable) {', source.index('esp_err_t espnow_init('))
end = source.index('\n    uint32_t *enable', start)
allocation = source[start:end]
header = r'''
#include <cassert>
#include <cstdlib>
#include <cstring>
#include "pairing/setup_config.hpp"
#include "pairing/security_guard.h"
using esp_err_t=int;
constexpr int ESP_OK=0, APP_KEY_LEN=32, KEY_LEN=16;
struct espnow_sec_t { bool initialized; unsigned tag_len; };
espnow_sec_t *g_espnow_sec=nullptr,*g_espnow_dec=nullptr;
unsigned char g_espnow_sec_key[32]{},g_espnow_dec_key[32]{};
bool g_read_from_nvs=false;
unsigned allocations=0; bool allocation_failure=false;
void *allocate(size_t n) { ++allocations; return allocation_failure ? nullptr : calloc(1,n); }
#define ESP_MALLOC(n) static_cast<espnow_sec_t*>(allocate(n))
#define ESP_PARAM_CHECK(p) do { if (!(p)) return -1; } while(0)
#define ESP_LOG_BUFFER_HEX_LEVEL(...)
#define ESP_ERROR_RETURN(p,r,...) do { if(p) return r; } while(0)
void espnow_sec_init(espnow_sec_t *p) { assert(p); p->initialized=true; }
unsigned key_set_calls=0;
int espnow_sec_setkey(espnow_sec_t *p,unsigned char*) { ++key_set_calls; return p && p->initialized ? 0 : -1; }
int espnow_storage_set(const char*,unsigned char*,int) { return 0; }
int espnow_storage_get(const char*,unsigned char*,int) { return -1; }
struct Config { bool sec_enable=false,forward_enable=true;
 struct { bool ack=false,group=false; } receive_enable;
 unsigned send_max_timeout=0; };
void initialize_contexts(const Config *config) {
'''
responder = (root / 'src/security/src/espnow_security_responder.c').read_text()
responder_stubs = r'''
using protocomm_t=int;
using espnow_addr_t=unsigned char[6];
constexpr int ESPNOW_DATA_LEN=218, ESPNOW_SEC_VER_NONE=0, ESP_ERR_NO_MEM=-2;
constexpr int ESPNOW_SEC_TYPE_REQUEST=0,ESPNOW_SEC_TYPE_REST=5,ESPNOW_SEC_TYPE_KEY=3;
constexpr int ESPNOW_SEC_TYPE_HANDSHAKE=2, ESPNOW_SEC_TYPE_KEY_RESP=4;
constexpr int ESPNOW_DATA_TYPE_SECURITY=1, ESP_EVENT_ESPNOW=1, ESP_EVENT_ESPNOW_SEC_FAIL=1;
struct espnow_sec_packet_t { unsigned char type,size; unsigned char data[230]; };
struct { unsigned char sec_ver=0; unsigned char client_mac[6]{}; } g_sec_info;
protocomm_t *g_espnow_pc=nullptr;
unsigned pairing_handshakes=0, parser_calls=0, send_calls=0, sessions=0;
struct espnow_frame_head_t { int retransmit_count; bool broadcast,filter_adjacent_channel; int forward_ttl; };
#define ESPNOW_ADDR_IS_EMPTY(p) (!(p)[0] && !(p)[1] && !(p)[2] && !(p)[3] && !(p)[4] && !(p)[5])
#define ESPNOW_ADDR_IS_EQUAL(a,b) (!memcmp(a,b,6))
#define ESP_FREE(p) free(p)
#define ESP_LOGD(...)
#define ESP_LOGE(...)
#define ESP_LOGW(...)
#define pdMS_TO_TICKS(x) (x)
int protocomm_open_session(protocomm_t*,unsigned) { ++sessions; return 0; }
int protocomm_close_session(protocomm_t*,unsigned) { --sessions; return 0; }
int protocomm_req_handle(protocomm_t*,const char*,unsigned,const unsigned char*,unsigned,unsigned char **out,ssize_t *len) {
 ++parser_calls; *len=1; *out=static_cast<unsigned char*>(malloc(1)); **out=0; return 0;
}
int espnow_send(int,const unsigned char*,void*,ssize_t,espnow_frame_head_t*,int) { ++send_calls; return 0; }
int esp_event_post(int,int,void*,size_t,int) { return 0; }
void pairing_response_complete(const unsigned char*,int) {}
'''
initiator = (root / 'src/security/src/espnow_security_initiator.c').read_text()
entry = initiator[initiator.index('esp_err_t espnow_sec_initiator_start('):]
entry = entry[:entry.index('#if ESP_IDF_VERSION')] + 'return 0;\n}\n'
public_start=initiator[initiator.index('esp_err_t espnow_sec_initiator_start('):initiator.index('esp_err_t espnow_sec_initiator_stop()')]
cleanup=public_start[public_start.index('    espnow_set_config_for_data_type(ESPNOW_DATA_TYPE_SECURITY, 0, NULL);'):public_start.rindex('    return ret;')]
assert 'vQueueDelete' not in cleanup and 'g_sec_queue = NULL' not in cleanup
queue_stubs = r'''
constexpr int ESP_ERR_INVALID_STATE=-3,ESP_ERR_INVALID_ARG=-4,ESP_FAIL=-5,pdPASS=1;
struct wifi_pkt_rx_ctrl_t {};
unsigned peer_mutations=0;
int espnow_add_peer(unsigned char*,void*) { ++peer_mutations; return 0; }
int espnow_del_peer(unsigned char*) { ++peer_mutations; return 0; }
int espnow_sec_info(const unsigned char*) { return 0; }
int espnow_sec_reset_info(const unsigned char*) { return 0; }
struct espnow_sec_result_t { unsigned unfinished_num=1; };
struct espnow_sec_data_t { unsigned char src_addr[6]; void *data; size_t size; };
bool pairing_attempt_used=false;
unsigned pairing_phase=0;
unsigned char pairing_peer[6]={2,1,1,1,1,1};
bool draining=false;
int espnow_set_config_for_data_type(int,int,void*) { return 0; }
int queue_object;
void *g_sec_queue=&queue_object;
espnow_sec_data_t queued{};
int xQueueSend(void *queue,espnow_sec_data_t *item,int) { assert(queue==&queue_object); queued=*item; return pdPASS; }
'''
def loop_header(text, marker):
    start=text.index(marker)
    brace=text.index('{',start)
    return text[start:brace]
retry_header=loop_header(initiator,'for (int i = 0; i < retry_count &&')
receive_header=loop_header(initiator,'while ((xTaskGetTickCount() - pairing_start')
# Execute everything between the actual receive and first packet copy. Removing
# the expiry check, or moving it below the copy, therefore changes this fixture.
receive_start=initiator.index(receive_header)+len(receive_header)
receive_body_start=initiator.index('{',receive_start)+1
receive_prefix=initiator[receive_body_start:initiator.index('                memcpy(src_addr,',receive_body_start)]
# Bind the outer fixture to the real secure receive branch, independently of the
# guard's spelling/location. It includes the IV copy, key update and decrypt.
recv_function=function('espnow_recv_process')
outer_start=recv_function.index('                    uint8_t key_info[APP_KEY_LEN];')
outer_end=recv_function.index('\n                } else {',outer_start)
outer_region=recv_function[outer_start:outer_end]
outer_fixture=r'''
constexpr unsigned IV_LEN=8,TAG_LEN=4,ESPNOW_PAYLOAD_LEN=230;
unsigned decrypt_calls=0;
int espnow_get_dec_key(unsigned char *key) { memset(key,0,32); return 0; }
int espnow_sec_auth_decrypt(espnow_sec_t*,const unsigned char*,size_t,unsigned char*,size_t,size_t*,unsigned) {
 ++decrypt_calls; return 0;
}
#define ESP_ERROR_GOTO(condition,label,...) do { if(condition) goto label; } while(0)
bool outer_path(size_t length) {
 struct Packet { size_t size; unsigned char *payload; } packet{length,static_cast<unsigned char*>(malloc(length?length:1))};
 auto *espnow_data=&packet;
 unsigned char data[ESPNOW_PAYLOAD_LEN]{}; size_t size=0; int ret=0;
''' + outer_region + r'''
 free(packet.payload); return true;
EXIT:
 free(packet.payload); return false;
}
void outer_test() {
 for(size_t n=0;n<=12;++n) {
  unsigned before_keys=key_set_calls,before_decrypt=decrypt_calls;
  assert(!outer_path(n));
  assert(key_set_calls==before_keys && decrypt_calls==before_decrypt);
 }
 assert(outer_path(13)); assert(outer_path(230));
 unsigned before_keys=key_set_calls,before_decrypt=decrypt_calls;
 assert(!outer_path(231));
 assert(key_set_calls==before_keys && decrypt_calls==before_decrypt);
}
''' 
deadline_fixture = r'''
using TickType_t=unsigned;
unsigned fake_now=0;
unsigned xTaskGetTickCount() { return fake_now; }
int xQueueReceive(void*,espnow_sec_data_t *item,unsigned wait) {
 if (draining) {
  if(!queued.data) return 0;
  *item=queued; queued.data=nullptr; return pdPASS;
 }
 assert(wait<=30000-fake_now); ++fake_now;
 item->data=malloc(3); item->size=3;
 auto *bytes=static_cast<unsigned char*>(item->data); bytes[0]=2; bytes[1]=1; bytes[2]=0;
 return pdPASS;
}
bool g_sec_initiator_flag=true;
void deadline_test() {
 unsigned pairing_start=0; int retry_count=100; unsigned recv_ticks=100;
 espnow_sec_result_t res; auto *result=&res;
 espnow_sec_data_t sec_data{};
 unsigned retries=0,parsed=0;
 fake_now=29998;
''' + retry_header + r''' {
 ++retries;
''' + receive_header + ' {' + receive_prefix + r'''

 // Structurally valid but rejected Security1 input cannot prolong the deadline.
 ++parsed; free(sec_data.data);
 }
 }
 assert(fake_now==30000 && retries==1 && parsed==1);
}
'''
teardown_fixture='void teardown() { draining=true;\n'+cleanup+'\ndraining=false; }\n'
main = r'''
int main() {
 deadline_test();
 unsigned char key[32]{}, retrieved[32]{};
 for(unsigned i=0;i<32;++i) key[i]=i+1;
 Config defaults;
 initialize_contexts(&defaults);
 assert(!g_espnow_sec && !g_espnow_dec);
 assert(espnow_set_key(key)!=0 && espnow_set_dec_key(key)!=0);
 auto config=pairing::setup_config(defaults,100);
 initialize_contexts(&config);
 assert(g_espnow_sec && g_espnow_dec);
 // These are the two actual calls gating responder KEY_RESP and SEC_OK.
 assert(espnow_set_key(key)==0 && espnow_set_dec_key(key)==0);
 assert(espnow_get_key(retrieved)==0 && !memcmp(key,retrieved,32));
 outer_test();
 free(g_espnow_sec); free(g_espnow_dec);
 unsigned char peer_a[6]={2,1,1,1,1,1},peer_b[6]={2,2,2,2,2,2},packet[3]={2,1,0};
 memcpy(g_sec_info.client_mac,peer_a,6);
 unsigned before=allocations;
 assert(espnow_sec_handle("espnow-session",2,peer_b,packet,3)==0);
 assert(allocations==before && parser_calls==0 && sessions==0 && send_calls==0);
 memset(g_sec_info.client_mac,0,6); allocation_failure=true;
 assert(espnow_sec_handle("espnow-session",2,peer_a,packet,3)==ESP_ERR_NO_MEM);
 assert(parser_calls==0 && sessions==0 && send_calls==0 && ESPNOW_ADDR_IS_EMPTY(g_sec_info.client_mac));
 allocation_failure=false;
 assert(espnow_sec_handle("espnow-session",2,peer_a,packet,3)==0);
 assert(parser_calls==1 && sessions==1 && send_calls==1);
 // The exact public SDK entry rejects a retry, including while a late callback is pending.
 espnow_sec_result_t result{};
 const unsigned char peers[1][6]={{2,1,1,1,1,1}};
 assert(espnow_sec_initiator_start(key,"shape-only-test-input",peers,1,&result)==0);
 // Simulate unregister/drain while an already-dispatched callback has not completed.
 // Queue storage is retained; callback can safely enqueue after teardown.
 teardown(); assert(g_sec_queue==&queue_object);
 wifi_pkt_rx_ctrl_t rx{};
 unsigned char handshake[3]={2,1,0};
 assert(espnow_initiator_sec_process(peer_a,handshake,3,&rx)==0);
 assert(queued.size==3 && queued.data);
 void *pending=queued.data;
 before=allocations;
 unsigned char malformed[300]{}; malformed[0]=2; malformed[1]=1;
 for(size_t n=0;n<300;++n) {
   if(n==3) continue;
   assert(espnow_initiator_sec_process(peer_a,malformed,n,&rx)!=0);
 }
 assert(allocations==before && queued.data==pending);
 unsigned parsers_before=parser_calls, peers_before=peer_mutations;
 for(size_t n=0;n<300;++n) {
   if(n==3) continue;
   assert(espnow_sec_responder_process(peer_a,malformed,n,&rx)!=0);
 }
 assert(parser_calls==parsers_before && allocations==before && peer_mutations==peers_before);
 assert(espnow_sec_responder_process(peer_b,handshake,3,&rx)==ESP_ERR_INVALID_STATE);
 assert(parser_calls==parsers_before && allocations==before && peer_mutations==peers_before);


 assert(espnow_sec_initiator_start(key,"shape-only-test-input",peers,1,&result)==ESP_ERR_INVALID_STATE);
 free(queued.data); queued.data=nullptr;

}
'''
with tempfile.TemporaryDirectory() as tmp:
    cpp = pathlib.Path(tmp) / 'context.cpp'
    cpp.write_text(header + allocation + '\n}\n' + '\n'.join(function(n) for n in
        ['espnow_set_key', 'espnow_set_dec_key', 'espnow_get_key']) + responder_stubs + function('espnow_sec_handle',responder) + queue_stubs + 'int espnow_sec_handle(const char *n,uint8_t t,const uint8_t *p,void *d,size_t s) { return espnow_sec_handle(n,t,p,static_cast<const uint8_t*>(d),s); }\n' + function('espnow_initiator_sec_process',initiator) + function('espnow_sec_responder_process',responder) + entry + deadline_fixture + teardown_fixture + outer_fixture + main)
    executable = pathlib.Path(tmp) / 'check'
    includes = pathlib.Path(__file__).resolve().parents[1] / 'include'
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                    '-I'+str(includes),str(cpp),'-o',str(executable)],check=True)
    subprocess.run([str(executable)],check=True)
