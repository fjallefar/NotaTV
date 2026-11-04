// Genbrugelig Widevine-CDM-vaert: indlaeser modulet, henter noegler fra en
// licensserver, og dekrypterer enkelt-samples via cdm->Decrypt().
// Bruges af baade license-testen og segment-dekrypteringen.
#pragma once
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <sys/time.h>
#include <curl/curl.h>
#include "cdm/content_decryption_module.h"

namespace wv {

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
inline std::vector<uint8_t> b64decode(const std::string& in) {
  int T[256]; for (int i = 0; i < 256; i++) T[i] = -1;
  for (int i = 0; i < 64; i++) T[(unsigned char)B64[i]] = i;
  std::vector<uint8_t> out; int val = 0, bits = -8;
  for (unsigned char c : in) {
    if (T[c] == -1) continue;
    val = (val << 6) + T[c]; bits += 6;
    if (bits >= 0) { out.push_back((uint8_t)((val >> bits) & 0xFF)); bits -= 8; }
  }
  return out;
}

inline size_t curl_wr(void* p, size_t s, size_t n, void* u) {
  ((std::string*)u)->append((char*)p, s * n); return s * n;
}
// HTTP POST med raa krop + valgfri ekstra header (fx x-dt-auth-token)
inline bool http_post(const std::string& url, const std::vector<uint8_t>& body,
                      const std::string& extra_header, std::string& out, long& code) {
  CURL* c = curl_easy_init(); if (!c) return false;
  out.clear();
  struct curl_slist* h = nullptr;
  h = curl_slist_append(h, "Content-Type: application/octet-stream");
  if (!extra_header.empty()) h = curl_slist_append(h, extra_header.c_str());
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
  curl_easy_setopt(c, CURLOPT_POST, 1L);
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.data());
  curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_wr);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
  CURLcode r = curl_easy_perform(c);
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  curl_slist_free_all(h); curl_easy_cleanup(c);
  return r == CURLE_OK;
}
inline bool http_post2(const std::string& url, const std::string& body,
                       const std::vector<std::string>& headers, std::string& out, long& code) {
  CURL* c = curl_easy_init(); if (!c) return false;
  out.clear();
  struct curl_slist* h = nullptr;
  for (auto& s : headers) h = curl_slist_append(h, s.c_str());
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
  curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_wr);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
  CURLcode r = curl_easy_perform(c);
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  curl_slist_free_all(h); curl_easy_cleanup(c);
  return r == CURLE_OK;
}
inline bool http_get(const std::string& url, std::string& out, long& code) {
  CURL* c = curl_easy_init(); if (!c) return false;
  out.clear();
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_wr);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  CURLcode r = curl_easy_perform(c);
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(c);
  return r == CURLE_OK;
}

class BufferImpl : public cdm::Buffer {
 public:
  explicit BufferImpl(uint32_t cap) : d_(cap), n_(0) {}
  void Destroy() override { delete this; }
  uint32_t Capacity() const override { return (uint32_t)d_.size(); }
  uint8_t* Data() override { return d_.data(); }
  void SetSize(uint32_t s) override { n_ = s; }
  uint32_t Size() const override { return n_; }
 private:
  std::vector<uint8_t> d_; uint32_t n_;
};
class DecryptedBlockImpl : public cdm::DecryptedBlock {
 public:
  void SetDecryptedBuffer(cdm::Buffer* b) override { buf_ = b; }
  cdm::Buffer* DecryptedBuffer() override { return buf_; }
  void SetTimestamp(int64_t t) override { ts_ = t; }
  int64_t Timestamp() const override { return ts_; }
 private:
  cdm::Buffer* buf_ = nullptr; int64_t ts_ = 0;
};

struct State {
  std::string session_id;
  bool got_message = false, keys_changed = false, have_usable_key = false;
  bool initialized = false, init_ok = false, rejected = false;
  cdm::MessageType msg_type = cdm::kLicenseRequest;
  std::vector<uint8_t> message;
  std::string key_report, reject_msg;
};
inline State& S() { static State s; return s; }

class HostImpl : public cdm::Host_10 {
 public:
  cdm::Buffer* Allocate(uint32_t c) override { return new BufferImpl(c); }
  void SetTimer(int64_t, void*) override {}
  cdm::Time GetCurrentWallTime() override {
    struct timeval tv; gettimeofday(&tv, nullptr);
    return (cdm::Time)tv.tv_sec + tv.tv_usec / 1e6;
  }
  void OnInitialized(bool ok) override { S().initialized = true; S().init_ok = ok; }
  void OnResolveKeyStatusPromise(uint32_t, cdm::KeyStatus) override {}
  void OnResolveNewSessionPromise(uint32_t, const char* s, uint32_t n) override { S().session_id.assign(s, n); }
  void OnResolvePromise(uint32_t) override {}
  void OnRejectPromise(uint32_t, cdm::Exception, uint32_t, const char* m, uint32_t n) override {
    S().rejected = true; S().reject_msg.assign(m, n);
  }
  void OnSessionMessage(const char* s, uint32_t sn, cdm::MessageType t, const char* m, uint32_t mn) override {
    S().session_id.assign(s, sn); S().msg_type = t;
    S().message.assign((const uint8_t*)m, (const uint8_t*)m + mn); S().got_message = true;
  }
  void OnSessionKeysChange(const char*, uint32_t, bool u, const cdm::KeyInformation* k, uint32_t n) override {
    S().keys_changed = true; S().have_usable_key = u; char b[8];
    for (uint32_t i = 0; i < n; i++) {
      std::string kid; for (uint32_t j = 0; j < k[i].key_id_size; j++) { snprintf(b, 8, "%02x", k[i].key_id[j]); kid += b; }
      S().key_report += "  key " + kid + (k[i].status == cdm::kUsable ? " -> USABLE\n" : " -> IKKE-brugbar\n");
    }
  }
  void OnExpirationChange(const char*, uint32_t, cdm::Time) override {}
  void OnSessionClosed(const char*, uint32_t) override {}
  void SendPlatformChallenge(const char*, uint32_t, const char*, uint32_t) override {}
  void EnableOutputProtection(uint32_t) override {}
  void QueryOutputProtectionStatus() override {}
  void OnDeferredInitializationDone(cdm::StreamType, cdm::Status) override {}
  cdm::FileIO* CreateFileIO(cdm::FileIOClient*) override { return nullptr; }
  void RequestStorageId(uint32_t) override {}
};
inline HostImpl& host() { static HostImpl h; return h; }

extern "C" inline void* GetCdmHost(int v, void*) { return v == 10 ? (void*)&host() : nullptr; }

// Indlaeser modulet, aabner session ud fra PSSH, og henter noegler.
class Cdm {
 public:
  cdm::ContentDecryptionModule_10* cdm_ = nullptr;
  bool load(const char* so) {
    void* lib = dlopen(so, RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { fprintf(stderr, "[cdm] dlopen: %s\n", dlerror()); return false; }
    auto Init = (bool(*)(const uint8_t*, uint32_t))dlsym(lib, "InitializeCdmModule_4");
    typedef void*(*CreateFn)(int, const char*, uint32_t, GetCdmHostFunc, void*);
    auto Create = (CreateFn)dlsym(lib, "CreateCdmInstance");
    if (!Init || !Create) { fprintf(stderr, "[cdm] mangler symbol\n"); return false; }
    Init(nullptr, 0);
    cdm_ = (cdm::ContentDecryptionModule_10*)Create(10, "com.widevine.alpha", 18, GetCdmHost, nullptr);
    if (!cdm_) { fprintf(stderr, "[cdm] CreateCdmInstance=null\n"); return false; }
    cdm_->Initialize(false, false, false);
    return true;
  }
  // PSSH -> licens-challenge -> licensserver -> noegler.
  // Kan kaldes flere gange (en session pr. PSSH); noeglerne akkumuleres i CDM'en,
  // saa baade 720p- og 432p-tierens KID bliver licenseret (de har hver sin).
  bool acquire(const std::vector<uint8_t>& pssh, const std::string& lic_url,
               const std::string& auth_header) {
    S().rejected = false; S().got_message = false; S().have_usable_key = false;
    cdm_->CreateSessionAndGenerateRequest(1, cdm::kTemporary, cdm::kCenc, pssh.data(), (uint32_t)pssh.size());
    if (S().rejected || !S().got_message) { fprintf(stderr, "[cdm] ingen challenge\n"); return false; }
    S().got_message = false;
    std::string resp; long code = 0;
    if (!http_post(lic_url, S().message, auth_header, resp, code) || code != 200) {
      fprintf(stderr, "[cdm] licensserver HTTP %ld\n", code); return false;
    }
    if (!resp.empty() && resp[0] == '{') {               // DRMtoday: {"license":"b64"}
      size_t k = resp.find("\"license\":\""); if (k == std::string::npos) return false;
      k += 11; size_t e = resp.find('"', k);
      auto lic = b64decode(resp.substr(k, e - k));
      resp.assign((const char*)lic.data(), lic.size());
    }
    cdm_->UpdateSession(2, S().session_id.data(), (uint32_t)S().session_id.size(),
                        (const uint8_t*)resp.data(), (uint32_t)resp.size());
    return S().have_usable_key;
  }
  // Dekrypter ét sample (CENC/AES-CTR) med subsample-opdeling. iv udfyldes til 16.
  bool decrypt(const uint8_t* data, uint32_t size, const uint8_t* key_id, uint32_t kid_sz,
               const uint8_t* iv, uint32_t iv_sz,
               const std::vector<cdm::SubsampleEntry>& subs, std::vector<uint8_t>& out) {
    uint8_t iv16[16] = {0}; memcpy(iv16, iv, iv_sz > 16 ? 16 : iv_sz);
    cdm::InputBuffer_2 in; memset(&in, 0, sizeof in);
    in.data = data; in.data_size = size;
    in.encryption_scheme = cdm::EncryptionScheme::kCenc;
    in.key_id = key_id; in.key_id_size = kid_sz;
    in.iv = iv16; in.iv_size = 16;
    in.subsamples = subs.data(); in.num_subsamples = (uint32_t)subs.size();
    in.timestamp = 0;
    DecryptedBlockImpl db;
    cdm::Status st = cdm_->Decrypt(in, &db);
    if (st != cdm::kSuccess || !db.DecryptedBuffer()) {
      fprintf(stderr, "[cdm] Decrypt status=%d\n", (int)st); return false;
    }
    cdm::Buffer* b = db.DecryptedBuffer();
    out.assign(b->Data(), b->Data() + b->Size());
    b->Destroy();
    return true;
  }
};

} // namespace wv
