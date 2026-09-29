/*
 * srt_keygen.c ― SRTオプションキー発行ツール(社内専用・客先には配らない)
 *
 *   鍵ペア作成(最初の1回だけ):
 *     ./srt_keygen --new-keypair=<保管フォルダ>
 *        → <保管フォルダ>/srt_license_private.pem (秘密鍵・社外秘・紛失厳禁)
 *        → <保管フォルダ>/srt_license_pub.h       (公開鍵・ソースに同梱してビルド)
 *   キー発行:
 *     ./srt_keygen --priv=<秘密鍵.pem> --mac=00:11:22:33:44:55 --features=ip,ngn
 *   キー確認(組み込み公開鍵で検証):
 *     ./srt_keygen --verify=<キー> --mac=00:11:22:33:44:55
 *
 * features: ip=SRT(IP接続)  ngn=SRT(NGN/データコネクト・CLEAR)  enc=SRT暗号化ライセンス(本体Ver.8.0)  all=ip+ngn
 */
#include "srt_license.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <openssl/pem.h>

static const char *opt(const char*a,const char*k){ size_t n=strlen(k); if(strncmp(a,k,n)==0&&a[n]=='=') return a+n+1; return NULL; }

static int parse_features(const char*s){
    int f=0; char buf[128]; snprintf(buf,sizeof buf,"%s",s);
    for(char*t=strtok(buf,",");t;t=strtok(NULL,",")){
        if(strcmp(t,"ip")==0) f|=LIC_SRT_IP;
        else if(strcmp(t,"ngn")==0) f|=LIC_SRT_NGN;
        else if(strcmp(t,"enc")==0) f|=LIC_SRT_ENC;
        else if(strcmp(t,"all")==0) f|=LIC_SRT_IP|LIC_SRT_NGN;
        else { fprintf(stderr,"不明な機能: %s (ip/ngn/all)\n",t); return -1; }
    }
    return f;
}

static int new_keypair(const char*dir){
    char path[1024];
    EVP_PKEY*pk=NULL; EVP_PKEY_CTX*c=EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519,NULL);
    if(!c||EVP_PKEY_keygen_init(c)!=1||EVP_PKEY_keygen(c,&pk)!=1){ fprintf(stderr,"鍵生成失敗\n"); return 1; }
    EVP_PKEY_CTX_free(c);

    snprintf(path,sizeof path,"%s/srt_license_private.pem",dir);
    struct stat st; if(stat(path,&st)==0){ fprintf(stderr,"既に秘密鍵があります(上書きしません): %s\n",path); return 1; }
    FILE*f=fopen(path,"w"); if(!f){ perror(path); return 1; }
    PEM_write_PrivateKey(f,pk,NULL,NULL,0,NULL,NULL); fclose(f); chmod(path,0600);
    fprintf(stderr,"秘密鍵: %s\n",path);

    unsigned char pub[32]; size_t pl=32;
    EVP_PKEY_get_raw_public_key(pk,pub,&pl);
    snprintf(path,sizeof path,"%s/srt_license_pub.h",dir);
    f=fopen(path,"w"); if(!f){ perror(path); return 1; }
    fprintf(f,"/* SRTオプションキー検証用 公開鍵(Ed25519) ― srt_keygen --new-keypair で生成 */\n");
    fprintf(f,"#ifndef SRT_LICENSE_PUB_H\n#define SRT_LICENSE_PUB_H\nstatic const unsigned char LIC_PUBKEY[32]={");
    for(int i=0;i<32;i++) fprintf(f,"%s0x%02x",i?",":"",pub[i]);
    fprintf(f,"};\n/* WEB画面(custom.js)用 hex: ");
    for(int i=0;i<32;i++) fprintf(f,"%02x",pub[i]);
    fprintf(f," */\n#endif\n"); fclose(f);
    fprintf(stderr,"公開鍵: %s\n",path);
    EVP_PKEY_free(pk);
    return 0;
}

static int issue(const char*privpath,const char*devid,int features){
    FILE*f=fopen(privpath,"r"); if(!f){ perror(privpath); return 1; }
    EVP_PKEY*pk=PEM_read_PrivateKey(f,NULL,NULL,NULL); fclose(f);
    if(!pk){ fprintf(stderr,"秘密鍵を読めません\n"); return 1; }
    char msg[80]; int ml=lic_message(devid,features,msg,sizeof msg);
    unsigned char blob[LIC_BLOB_LEN]; size_t sl=64; blob[0]=(unsigned char)features;
    EVP_MD_CTX*ctx=EVP_MD_CTX_new();
    if(EVP_DigestSignInit(ctx,NULL,NULL,NULL,pk)!=1||EVP_DigestSign(ctx,blob+1,&sl,(const unsigned char*)msg,ml)!=1||sl!=64){
        fprintf(stderr,"署名失敗\n"); return 1; }
    EVP_MD_CTX_free(ctx); EVP_PKEY_free(pk);
    char key[160]; lic_b32_encode(blob,LIC_BLOB_LEN,key,sizeof key);
    /* 自己検証(公開鍵がこの秘密鍵と対になっているか) */
    if(lic_verify(key,devid)!=features)
        fprintf(stderr,"警告: 組み込み公開鍵(srt_license_pub.h)と秘密鍵が対になっていません\n");
    char d[64]; lic_describe(features,d,sizeof d);
    fprintf(stderr,"装置MAC=%s  機能=%s\n",devid,d);
    printf("%s\n",key);
    return 0;
}

int main(int argc,char**argv){
    const char*v,*newdir=NULL,*priv=NULL,*mac=NULL,*feat=NULL,*vkey=NULL;
    for(int i=1;i<argc;i++){
        if((v=opt(argv[i],"--new-keypair"))) newdir=v;
        else if((v=opt(argv[i],"--priv"))) priv=v;
        else if((v=opt(argv[i],"--mac"))) mac=v;
        else if((v=opt(argv[i],"--features"))) feat=v;
        else if((v=opt(argv[i],"--verify"))) vkey=v;
        else { fprintf(stderr,"不明: %s\n",argv[i]); return 1; }
    }
    if(newdir) return new_keypair(newdir);

    char devid[13];
    if(!mac||lic_normalize_mac(mac,devid)!=0){ fprintf(stderr,"--mac=<HDIPのMACアドレス> が必要です\n"); return 1; }
    if(vkey){
        int f=lic_verify(vkey,devid); char d[64]; lic_describe(f,d,sizeof d);
        printf("%s: 機能=%s\n", f?"有効":"無効", d);
        return f?0:2;
    }
    if(!priv||!feat){ fprintf(stderr,"--priv=<秘密鍵> --features=ip|ngn|all が必要です\n"); return 1; }
    int f=parse_features(feat); if(f<=0) return 1;
    return issue(priv,devid,f);
}
