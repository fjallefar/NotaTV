// wvserve - lokal HTTP-proxy der lader mpv afspille en TV2 Widevine-DASH-stream.
// Henter noegler EN gang ved start (holder hele filmen), omskriver MPD'en til
// ren video, og dekrypterer hvert segment on-the-fly via Widevine-modulet.
//
// brug: wvserve <so> <lic-url> <auth-token> <pssh-b64> <mpd-url> [port]
// mpv afspiller derefter http://127.0.0.1:<port>/manifest.mpd

#include "cdm_host.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <thread>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <signal.h>
#include <pthread.h>
#include <sys/un.h>
#include <sys/time.h>
#include <cstring>
#include <cstdlib>
#include <atomic>

using namespace wv;

static uint32_t rd32(const uint8_t* p){return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3];}
static uint64_t rd64(const uint8_t* p){return ((uint64_t)rd32(p)<<32)|rd32(p+4);}

// q..q+n skal ligge inde i boxen (end): segmentet kommer fra en CDN, saa alt skal graenses
static bool fits(const uint8_t* q,const uint8_t* end,uint64_t n){
  if(q<=end&&(uint64_t)(end-q)>=n) return true;
  fprintf(stderr,"[serve] laesning uden for box\n"); return false;
}
static bool skip(const uint8_t*& q,const uint8_t* end,uint64_t n){
  if(!fits(q,end,n)) return false;
  q+=n; return true;
}

static const uint8_t* find_box(const uint8_t* p,const uint8_t* end,const char* type,
                               const uint8_t** pl,uint64_t* ps){
  while(p+8<=end){
    uint64_t sz=rd32(p); const char* t=(const char*)p+4; const uint8_t* hdr=p; uint32_t hs=8;
    if(sz==1){sz=rd64(p+8);hs=16;} else if(sz==0) sz=end-p;
    if(sz<hs||p+sz>end) break;
    if(!memcmp(t,type,4)){ if(pl)*pl=p+hs; if(ps)*ps=sz-hs; return hdr; }
    static const char* C[]={"moov","trak","mdia","minf","stbl","moof","traf","sinf","schi","mvex","edts",0};
    bool c=false; for(int i=0;C[i];i++) if(!memcmp(t,C[i],4)) c=true;
    const uint8_t* cp=nullptr;
    if(c) cp=p+hs;
    else if(!memcmp(t,"stsd",4)) cp=p+hs+8;
    else if(!memcmp(t,"encv",4)||!memcmp(t,"avc1",4)) cp=p+hs+78;   // video sample entry
    else if(!memcmp(t,"enca",4)||!memcmp(t,"mp4a",4)) cp=p+hs+28;   // audio sample entry
    if(cp){ const uint8_t* r=find_box(cp,p+sz,type,pl,ps); if(r) return r; }
    p+=sz;
  }
  return nullptr;
}

struct TrackKey { std::vector<uint8_t> kid; uint8_t iv_size=8; };

static Cdm g_cdm;
static std::mutex g_mtx;                 // CDM er ikke traadsikker
static std::string g_base;               // real base-url til segmenter
static std::map<std::string,TrackKey> g_keys;  // repid -> KID
static std::mutex g_keys_mtx;

// --- init-segment: encv->avc1, enca->mp4a, sinf->free; udlaes tenc (KID,iv_size) ---
static void patch_init(std::vector<uint8_t>& b, const std::string& repid){
  const uint8_t* pl; uint64_t pz;
  const uint8_t* tenc=find_box(b.data(),b.data()+b.size(),"tenc",&pl,&pz);
  if(tenc&&pz>=24){ TrackKey tk; tk.iv_size=pl[7]; tk.kid.assign(pl+8,pl+24);
    std::lock_guard<std::mutex> lk(g_keys_mtx); g_keys[repid]=tk; }
  else if(tenc) fprintf(stderr,"[serve] tenc for kort (%llu byte) - ingen KID\n",(unsigned long long)pz);
  for(const char* e:{"encv","enca"}){ const uint8_t* x=find_box(b.data(),b.data()+b.size(),e,&pl,&pz);
    if(x) memcpy((void*)(x+4), e[3]=='v'?"avc1":"mp4a",4); }
  for(const char* s:{"sinf"}){ const uint8_t* x; while((x=find_box(b.data(),b.data()+b.size(),s,&pl,&pz))) { memcpy((void*)(x+4),"free",4);} }
}

// --- media-segment: dekryptér alle samples in-place, omdoeb cenc-bokse ---
static bool decrypt_segment(std::vector<uint8_t>& seg, const std::string& repid){
  TrackKey tk; { std::lock_guard<std::mutex> lk(g_keys_mtx); auto it=g_keys.find(repid); if(it==g_keys.end()){fprintf(stderr,"[serve] ingen KID for %s\n",repid.c_str());return false;} tk=it->second; }
  uint8_t* S0=seg.data(); uint8_t* SE=S0+seg.size();
  const uint8_t *moofpl,*trunp,*sp; uint64_t z;
  const uint8_t* moof=find_box(S0,SE,"moof",&moofpl,&z); if(!moof) return true;
  uint64_t moof_off=moof-S0;
  if(!find_box(S0,SE,"trun",&trunp,&z)) return true;
  const uint8_t* trun_end=trunp+z;            // trun's egen payload-graense (z bruges til flere bokse nedenfor)
  if(tk.iv_size>16){fprintf(stderr,"[serve] ugyldig iv_size %u\n",(unsigned)tk.iv_size);return false;}
  if(!fits(trunp,trun_end,8)) return false;
  uint32_t trf=rd32(trunp)&0xffffff, n=rd32(trunp+4); const uint8_t* q=trunp+8; int32_t doff=0;
  if(n>(1u<<22)){fprintf(stderr,"[serve] %u samples i trun - afvist\n",n);return false;} // foer vektoren allokeres
  if(trf&0x1){ if(!fits(q,trun_end,4)) return false; doff=(int32_t)rd32(q);q+=4; }
  if(trf&0x4){ if(!fits(q,trun_end,4)) return false; q+=4; }
  uint32_t defsz=0; { const uint8_t* tfp; uint64_t tz; if(find_box(S0,SE,"tfhd",&tfp,&tz)){ uint32_t ff=rd32(tfp)&0xffffff; const uint8_t* r=tfp+8; if(ff&0x1)r+=8; if(ff&0x2)r+=4; if(ff&0x8)r+=4; if(ff&0x10&&fits(r,tfp+tz,4)){defsz=rd32(r);} } }
  std::vector<uint32_t> sizes(n,defsz);
  // hvert flag flytter q videre: stop hvis en raekke samples ikke kan vaere inde i trun'en
  for(uint32_t i=0;i<n;i++){
    if(trf&0x100&&!skip(q,trun_end,4)) return false;
    if(trf&0x200){ if(!fits(q,trun_end,4)) return false; sizes[i]=rd32(q); q+=4; }
    if(trf&0x400&&!skip(q,trun_end,4)) return false;
    if(trf&0x800&&!skip(q,trun_end,4)) return false;
  }
  const uint8_t* senc=find_box(S0,SE,"senc",&sp,&z); if(!senc) return true;
  const uint8_t* senc_end=sp+z;                // senc's egen payload-graense
  if(!fits(sp,senc_end,8)) return false;
  uint32_t sfl=rd32(sp)&0xffffff, cnt=rd32(sp+4); const uint8_t* e=sp+8;
  uint64_t off=moof_off+doff;
  std::lock_guard<std::mutex> lk(g_mtx);
  for(uint32_t i=0;i<n&&i<cnt;i++){
    if(!fits(e,senc_end,tk.iv_size)) return false;
    uint8_t iv[16]={0}; memcpy(iv,e,tk.iv_size); e+=tk.iv_size;
    std::vector<cdm::SubsampleEntry> subs;
    if(sfl&0x2){ if(!fits(e,senc_end,2)) return false; uint16_t sc=(e[0]<<8)|e[1]; e+=2; if(!fits(e,senc_end,(uint64_t)sc*6)) return false; for(uint16_t j=0;j<sc;j++){ cdm::SubsampleEntry s; s.clear_bytes=(e[0]<<8)|e[1]; s.cipher_bytes=rd32(e+2); e+=6; subs.push_back(s);} }
    else { cdm::SubsampleEntry s; s.clear_bytes=0; s.cipher_bytes=sizes[i]; subs.push_back(s); }
    while(!subs.empty()&&subs.back().cipher_bytes==0) subs.pop_back();
    uint32_t cover=0; bool anyc=false; for(auto&s:subs){cover+=s.clear_bytes+s.cipher_bytes; if(s.cipher_bytes)anyc=true;}
    if(!anyc){ off+=sizes[i]; continue; }
    if(off+cover>seg.size()){fprintf(stderr,"[serve] off+cover>size\n");return false;}
    std::vector<uint8_t> out;
    if(g_cdm.decrypt(S0+off,cover,tk.kid.data(),16,iv,tk.iv_size,subs,out)&&out.size()==cover)
      memcpy(S0+off,out.data(),out.size());
    else { fprintf(stderr,"[serve] sample %u dekrypt fejl (%s)\n",i,repid.c_str()); return false; }
    off+=sizes[i];
  }
  for(const char* bx:{"senc","saiz","saio","sbgp","sgpd"}){ const uint8_t* x; uint64_t pz; const uint8_t* pl;
    while((x=find_box(S0,SE,bx,&pl,&pz))) memcpy((void*)(x+4),"free",4); }
  return true;
}

// fjern alle <ContentProtection ...>...</ContentProtection> og self-closing
static void strip_cp(std::string& m){
  size_t p;
  while((p=m.find("<ContentProtection"))!=std::string::npos){
    size_t gt=m.find('>',p); if(gt==std::string::npos) break;
    if(m[gt-1]=='/'){ m.erase(p,gt-p+1); }
    else { size_t end=m.find("</ContentProtection>",gt); if(end==std::string::npos){m.erase(p,gt-p+1);} else m.erase(p,end+20-p); }
  }
}

static std::string repid_from_path(const std::string& path){
  // /s/<repid>/<fil>
  size_t a=path.find("/s/"); if(a==std::string::npos) return "";
  a+=3; size_t b=path.find('/',a); if(b==std::string::npos) return "";
  return path.substr(a,b-a);
}

static void send_all(int fd,const char* d,size_t n){ while(n){ ssize_t w=write(fd,d,n); if(w<=0)break; d+=w; n-=w; } }
static void respond(int fd,const char* status,const char* ctype,const std::string& body){
  char h[512]; int hn=snprintf(h,sizeof h,"HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",status,ctype,body.size());
  send_all(fd,h,hn); send_all(fd,body.data(),body.size());
}

static int g_port; static std::string g_mpd_rewritten;
static std::string g_access_token;   // used by the concurrency endpoint

static void handle(int fd){
  char buf[4096]; int n=read(fd,buf,sizeof buf-1); if(n<=0){close(fd);return;} buf[n]=0;
  char method[8]={0},path[2048]={0}; sscanf(buf,"%7s %2047s",method,path);
  std::string p(path);
  if(p=="/manifest.mpd"){ respond(fd,"200 OK","application/dash+xml",g_mpd_rewritten); close(fd); return; }
  if(p.rfind("/s/",0)==0){
    std::string real=g_base+p.substr(2);          // /s/<rest> -> base + /<rest>
    std::string data; long code;
    if(!http_get(real,data,code)||code!=200){ respond(fd,"502 Bad Gateway","text/plain","upstream"); close(fd); return; }
    std::vector<uint8_t> b(data.begin(),data.end());
    std::string rep=repid_from_path(p);
    bool subtitle = p.find(".vtt")!=std::string::npos || p.find(".ttml")!=std::string::npos ||
                    p.find(".srt")!=std::string::npos;
    bool is_init = !subtitle && p.find("init")!=std::string::npos;
    if(is_init) patch_init(b,rep);
    else if(!subtitle && !decrypt_segment(b,rep)){ respond(fd,"500 Internal","text/plain","decrypt"); close(fd); return; }
    std::string body((char*)b.data(),b.size());
    const char* ct="video/mp4";
    if(p.find(".vtt")!=std::string::npos) ct="text/vtt";
    else if(p.find(".ttml")!=std::string::npos) ct="application/ttml+xml";
    else if(p.find(".srt")!=std::string::npos) ct="text/plain; charset=utf-8";
    respond(fd,"200 OK",ct,body); close(fd); return;
  }
  respond(fd,"404 Not Found","text/plain","no"); close(fd);
}

// find "key":"value" efter from; returnerer value (uden escape-haandtering ud over \/)
static std::string jstr(const std::string& s,const std::string& key,size_t from=0){
  std::string pat="\""+key+"\":\""; size_t p=s.find(pat,from); if(p==std::string::npos) return "";
  p+=pat.size(); std::string out; for(size_t i=p;i<s.size()&&s[i]!='"';i++){ if(s[i]=='\\'&&i+1<s.size()){out+=s[i+1];i++;} else out+=s[i]; } return out;
}

static std::string g_conc_url, g_conc_token, g_guid;   // samtidigheds-session
static std::atomic<double> g_pos{-1.0};                 // seneste position fra mpv (sekunder)

// spoerg mpv's IPC om time-pos; -1 hvis mpv ikke koerer (fx foer player_run)
static double mpv_time_pos(){
  int fd=socket(AF_UNIX,SOCK_STREAM,0); if(fd<0) return -1;
  struct timeval tv{1,0};
  setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof tv);
  setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof tv);
  sockaddr_un a{}; a.sun_family=AF_UNIX;
  snprintf(a.sun_path,sizeof a.sun_path,"/tmp/notatv-mpv.sock");
  if(connect(fd,(sockaddr*)&a,sizeof a)<0){ close(fd); return -1; }
  const char* cmd="{\"command\":[\"get_property\",\"time-pos\"]}\n";
  if(write(fd,cmd,strlen(cmd))<0){ close(fd); return -1; }
  char buf[512]; ssize_t n=read(fd,buf,sizeof buf-1); close(fd);
  if(n<=0) return -1; buf[n]=0;
  char* p=strstr(buf,"\"data\":"); if(!p) return -1;
  p+=7; while(*p==' ') p++;
  if(*p=='n') return -1;              // null
  return atof(p);
}

// appen skriver den endelige position her, lige foer den stopper wvserve
static double posfile_pos(){
  const char* f=getenv("TV2_POSFILE"); if(!f||!*f) return -1;
  FILE* fp=fopen(f,"r"); if(!fp) return -1;
  double p=-1; if(fscanf(fp,"%lf",&p)!=1) p=-1;
  fclose(fp); return p;
}

// TV2 play_web_Playback: token+guid -> Widevine MPD-url, licens-url, licens-token
static bool resolve(const std::string& token,const std::string& guid,
                    std::string& mpd_url,std::string& lic_url,std::string& lic_token){
  g_guid=guid;
  std::string q="query q($guid:String!,$format:[Format]!,$drm:[Drm]!,$codec:[Codec],$resolution:[String],$dynamicRange:[DynamicRange],$deviceId:String!,$deviceDescription:String!){media(guid:$guid,deviceId:$deviceId,deviceDescription:$deviceDescription,capabilities:{format:$format,drm:$drm,codec:$codec,resolution:$resolution,dynamicRange:$dynamicRange},download:false){streams{format url drmLicense{drm url token}}concurrency{url body{token}}}}";
  std::string body="{\"operationName\":\"q\",\"query\":"+std::string("\"")+q+"\",\"variables\":{"
    "\"guid\":\""+guid+"\",\"deviceId\":\"00000000-0000-4000-0000-00000000\",\"deviceDescription\":\"Web\","
    "\"format\":[\"DASH_MULTI_PERIOD\",\"DASH_SINGLE_PERIOD\"],\"drm\":[\"WIDEVINE\",\"NONE\"],"
    "\"codec\":[\"H264\"],\"resolution\":[\"720P\"],\"dynamicRange\":[\"SDR\"]}}";
  std::vector<std::string> h={"Content-Type: application/json","Client-Platform: Web","Client-Platform-Audience: PLAY",
    "Client-Platform-AppVersion: 2026.11.7021017","Client-Platform-Class: chrome","Client-Platform-OS-Version: chrome",
    "authorization: Bearer "+token};
  const char* prof=getenv("TV2_PROFILE");
  if(prof&&*prof) h.push_back(std::string("Client-Platform-User-Profile: ")+prof);
  std::string resp; long code;
  if(!http_post2("https://play.tv2api.dk/",body,h,resp,code)||code!=200){ fprintf(stderr,"[resolve] HTTP %ld: %.200s\n",code,resp.c_str()); return false; }
  size_t wv=resp.find("\"WIDEVINE\""); if(wv==std::string::npos){ fprintf(stderr,"[resolve] ingen WIDEVINE-stream: %.300s\n",resp.c_str()); return false; }
  // stream-url: naermeste "url": foer WIDEVINE (i samme stream-objekt)
  size_t su=resp.rfind("\"url\":\"",wv); if(su==std::string::npos) return false;
  mpd_url=jstr(resp,"url",su);
  lic_url=jstr(resp,"url",wv);            // licens-url: foerste "url" efter drm:WIDEVINE
  lic_token=jstr(resp,"token",wv);
  // samtidigheds-session (efter streams i svaret)
  size_t cc=resp.find("\"concurrency\"");
  if(cc!=std::string::npos){ g_conc_url=jstr(resp,"url",cc); g_conc_token=jstr(resp,"token",cc); }
  return !mpd_url.empty()&&!lic_url.empty()&&!lic_token.empty();
}

// meld til/fra TV2's samtidigheds-tjeneste (UPDATE hvert 30. s, UNLOCK ved stop)
static void conc_post(const char* action){
  if(g_conc_url.empty()||g_conc_token.empty()) return;
  double pos=g_pos.load();
  if(!strcmp(action,"UNLOCK")){ double f=posfile_pos(); if(f>pos) pos=f; }
  std::string pv=pos>=0?std::to_string((long long)(pos+0.5)):"null";
  std::string body="{\"token\":\""+g_conc_token+"\",\"progress\":"+pv+",\"guid\":\""+g_guid+
    "\",\"userAction\":null,\"concurrencyAction\":\""+action+"\"}";
  std::vector<std::string> h={"Content-Type: application/json","Client-Platform: Web","Client-Platform-Audience: PLAY",
    "Client-Platform-AppVersion: 2026.11.7021017","Client-Platform-Class: chrome"};
  if(!g_access_token.empty()) h.push_back("Authorization: Bearer "+g_access_token);
  const char* prof=getenv("TV2_PROFILE");
  if(prof&&*prof) h.push_back(std::string("Client-Platform-User-Profile: ")+prof);
  std::string out; long code; http_post2(g_conc_url,body,h,out,code);
  fprintf(stderr,"[conc] %s pos=%.1f -> HTTP %ld\n",action,pos,code);
}

// hent ALLE Widevine-PSSH'er fra MPD'en (en pr. video-AdaptationSet: 720p og 432p
// har hver sin KID). For hver forekomst af Widevine-systemID'et tages den
// efterfoelgende <cenc:pssh>. Dubletter fjernes.
static std::vector<std::string> pssh_list_from_mpd(const std::string& mpd){
  std::vector<std::string> out;
  const std::string wvid="edef8ba9-79d6-4ace-a3c8-27dcd51d21ed";
  size_t pos=0;
  while((pos=mpd.find(wvid,pos))!=std::string::npos){
    size_t a=mpd.find("<cenc:pssh>",pos);
    if(a==std::string::npos){ pos+=wvid.size(); continue; }
    a+=11;
    size_t b=mpd.find("</cenc:pssh>",a);
    if(b==std::string::npos) break;
    std::string p=mpd.substr(a,b-a);
    if(std::find(out.begin(),out.end(),p)==out.end()) out.push_back(p);
    pos=b+12;
  }
  return out;
}

int main(int argc,char** argv){
  if(argc<4){ fprintf(stderr,"brug: %s <so> <tv2-token|@fil> <guid> [port]\n",argv[0]); return 2; }
  const char* so=argv[1]; std::string token=argv[2],guid=argv[3]; g_port=argc>4?atoi(argv[4]):8888;
  // "@<file>": the token is read from the file, so it never shows in the process list (ps, the log bundle)
  if(!token.empty()&&token[0]=='@'){
    FILE* tf=fopen(token.c_str()+1,"r"); token.clear();
    if(tf){ char b[8192]; size_t n=fread(b,1,sizeof b,tf); fclose(tf); token.assign(b,n); }
    while(!token.empty()&&(token.back()=='\n'||token.back()=='\r'||token.back()==' ')) token.pop_back();
    if(token.empty()){ fprintf(stderr,"resolve fejlede: intet token i %s\n",argv[2]+1); return 1; }
  }
  g_access_token=token;
  curl_global_init(CURL_GLOBAL_DEFAULT);
  std::string mpd_url,lic_url,lic_token;
  if(!resolve(token,guid,mpd_url,lic_url,lic_token)){ fprintf(stderr,"resolve fejlede\n"); return 1; }
  fprintf(stderr,"[serve] stream: %.80s\n[serve] licens: %s\n",mpd_url.c_str(),lic_url.c_str());
  g_base=mpd_url.substr(0,mpd_url.rfind('/'));
  std::string mpd; long code; if(!http_get(mpd_url,mpd,code)||code!=200){ fprintf(stderr,"mpd HTTP %ld\n",code); return 1; }
  auto psshs=pssh_list_from_mpd(mpd);
  if(psshs.empty()){ fprintf(stderr,"ingen pssh\n"); return 1; }
  std::string auth="x-dt-auth-token: "+lic_token;
  if(!g_cdm.load(so)) return 1;
  int ok=0;
  for(auto& pb:psshs){
    auto pssh=b64decode(pb);
    if(pssh.empty()) continue;
    if(g_cdm.acquire(pssh,lic_url,auth)) ok++;
    else fprintf(stderr,"[serve] acquire fejlede for en PSSH\n");
  }
  if(!ok){ fprintf(stderr,"ingen noegle\n%s",S().key_report.c_str()); return 1; }
  fprintf(stderr,"[serve] noegle OK (%d/%zu PSSH licenseret)\n%s",ok,psshs.size(),S().key_report.c_str());
  // samtidighed: meld til nu, UPDATE hvert 30. s med mpv's position, UNLOCK ved stop
  sigset_t sigs; sigemptyset(&sigs); sigaddset(&sigs,SIGTERM); sigaddset(&sigs,SIGINT);
  pthread_sigmask(SIG_BLOCK,&sigs,nullptr);
  conc_post("UPDATE");
  std::thread([sigs]{ int sig; for(;;){ if(sigwait(&sigs,&sig)==0){
    double p=mpv_time_pos(); if(p>=0) g_pos=p;
    conc_post("UNLOCK"); _exit(0);
  } } }).detach();
  std::thread([]{ for(;;){ sleep(30); double p=mpv_time_pos(); if(p>=0) g_pos=p; conc_post("UPDATE"); } }).detach();
  strip_cp(mpd);
  { size_t per=mpd.find("<Period"); std::string bu="<BaseURL>http://127.0.0.1:"+std::to_string(g_port)+"/s/</BaseURL>";
    if(per!=std::string::npos) mpd.insert(per,bu); }
  g_mpd_rewritten=mpd;
  int s=socket(AF_INET,SOCK_STREAM,0); int one=1; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
  sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); a.sin_port=htons(g_port);
  if(bind(s,(sockaddr*)&a,sizeof a)<0){ perror("bind"); return 1; }
  listen(s,16);
  fprintf(stderr,"[serve] klar: http://127.0.0.1:%d/manifest.mpd\n",g_port);
  for(;;){ int c=accept(s,0,0); if(c<0) continue; std::thread(handle,c).detach(); }
}
