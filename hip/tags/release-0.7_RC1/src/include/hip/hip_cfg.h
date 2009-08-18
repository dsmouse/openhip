#ifndef _HIPSPD_H_
#define _HIPSPD_H_
#include <netinet/in.h>
#include <netinet/ether.h>
#include <map>
#include <set>
#include <string>
#include <list>
#include <openssl/ssl.h>
#include <openssl/engine.h>
#include "hip_types.h"

using namespace std;

class certInfo
{
public:
  certInfo(char *hit) { _hit = hit; time(&_ts);};
  ~certInfo();
   time_t getTs() { return _ts; };
   const char *getHit() { return _hit.c_str(); };
  certInfo(){};

private:
  string _hit;
  time_t _ts;
};

class hitPair
{
public:
  hitPair(const hip_hit hit1, const hip_hit hit2);
  void print() const;
  bool operator<(const hitPair & hp) const;

public:
  hip_hit _hit1;
  hip_hit _hit2;
};

struct hp_compare
{
bool operator ()(const hitPair & hp1, const hitPair & hp2)
{
  return hp1 < hp2;
}
};


class hipCfg{
public:
  hipCfg();
  virtual ~hipCfg(){};
  int hit_peer_allowed(const hip_hit hit1, const hip_hit hit2);
  int peers_allowed(hip_hit *hits1, hip_hit *hits2, int max_cnt);
  int legacyNodeToEndbox(const struct sockaddr *host, struct sockaddr *eb);
  int endbox2Llip(const struct sockaddr *eb, struct sockaddr *llip);
  int getLegacyNodesByEndbox(const struct sockaddr *eb,
			     struct sockaddr_storage *hosts, int size);
  int getLocalCertUrl(char *url, unsigned int size);
  int getPeerNodes(struct peer_node *peerNodes, unsigned int max_count);
  hi_node *getMyHostId(){ return _hostid;};
  virtual int verifyCert(const char *url, const hip_hit hit) = 0;
  virtual int postLocalCert(const char *hit) = 0;
  virtual int loadCfg(struct hip_conf *hc) = 0;
  virtual int closeCfg() = 0;
  static int hit2hitstr(char *hit_str, const hip_hit hit);
  static int hitstr2lsistr(char *lsi_str, char *hit_str);
  static int addr_to_str(const struct sockaddr *addr, char *data, int len);
  static int hitstr2hit(hip_hit hit, char *hit_str);
  static int hex_to_bin(char *src, char *dst, int dst_len);
  static int str_to_addr(const char *data, struct sockaddr *addr);

protected:
  ENGINE *engine_init(const char *pin);
  int load_engine_fn(ENGINE *e, const char *engine_id,
                   const char **pre_cmds, int pre_num,
                   const char **post_cmds, int post_num);

  void engine_teardown(ENGINE *e);
  SSL_CTX *ssl_ctx_init(ENGINE *e, const char *pin);
  int init_ssl_context();
  int verify_certificate(X509 *cert);
  static int callb(int rc, X509_STORE_CTX *ctx);
  int hi_to_hit(hi_node *hi, hip_hit hit);
  int khi_hi_input(hi_node *hi, __u8 *out);
  int bn2bin_safe(const BIGNUM *a, unsigned char *to, int len);
  int khi_encode_n(__u8 *in, int len, __u8 *out, int n);
  int mkHIfromSc();
  int mkHIfromPkey(RSA *rsa, DSA *dsa,  hi_node *hostid);
  int getEndboxMapsFromLocalFile();

protected:
  map <string, string> _legacyNode2EndboxMap;
  map <string, string> _endbox2LlipMap; /* endbox (LSI) to Llip (BCWIN) mapping */
  string _localCertUrl;
  map <string, certInfo> _certs; /* cached certificates data indexed by cert url*/
  set <hitPair, hp_compare> _allowed_peers; /* pairs of hits allowed to start HIP base exchange */
  struct hip_conf *_hcfg;
  map <string, struct peer_node *> _hit_to_peers; /* configured peers indexed by hit string */
  string _scPrivKeyID;
  string _scPin;
  string _scCert;
  hi_node *_hostid;
  SSL *_ssl;
  X509_STORE *_store;
  RSA *_rsa;
  DSA *_dsa;
};

#endif
