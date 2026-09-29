/*
 * srt_license.h ― SRTオプションキー(装置ごと・電子署名式)
 *
 * 配るソフトは全員同じ1本。機能はキーで有効化する。
 *   キー = base32( [機能ビット 1byte][Ed25519署名 64byte] )
 *   署名対象 = "HDIPSRT1|<装置ID>|<機能ビット2桁hex>"
 *   装置ID   = HDIP-3000V本体のLAN1 MACアドレス12桁(大文字hex)。
 * キーはHDIPのWEB画面(Audio→SRT)に入力し、HDIP内 /opt/RF/websetting/srt.cfg に保存される。
 * 検証には公開鍵(srt_license_pub.h)だけを使う。秘密鍵は社内の srt_keygen 側にしか無いので、
 * ソースを配布してもキーは偽造できない。別の装置にキーをコピーしても装置IDが違うので無効。
 */
#ifndef SRT_LICENSE_H
#define SRT_LICENSE_H

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <openssl/evp.h>
#include "srt_license_pub.h"

#define LIC_SRT_IP   0x01   /* SRT(IP接続モード)            */
#define LIC_SRT_NGN  0x02   /* SRT(NGN/データコネクト・CLEAR) */
#define LIC_SRT_ENC  0x04   /* SRT暗号化ライセンス(本体Ver.8.0のパスフレーズ暗号化・仕様書8.0.2) */

#define LIC_BLOB_LEN 65

static const char LIC_B32[]="ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

/* base32(パディング無し)。8文字毎に '-' を入れる。 */
static inline void lic_b32_encode(const unsigned char*in,int n,char*out,int cap){
    int o=0,bits=0,cnt=0; unsigned v=0;
    for(int i=0;i<n;i++){
        v=(v<<8)|in[i]; bits+=8;
        while(bits>=5){ if(cnt&&cnt%8==0&&o<cap-1) out[o++]='-'; if(o<cap-1) out[o++]=LIC_B32[(v>>(bits-5))&31]; bits-=5; cnt++; }
    }
    if(bits>0){ if(cnt&&cnt%8==0&&o<cap-1) out[o++]='-'; if(o<cap-1) out[o++]=LIC_B32[(v<<(5-bits))&31]; }
    out[o]=0;
}

/* '-'・空白・改行は読み飛ばし、小文字も受け付ける。戻り値=復号バイト数(-1=不正文字) */
static inline int lic_b32_decode(const char*in,unsigned char*out,int cap){
    int o=0,bits=0; unsigned v=0;
    for(;*in;in++){
        int c=toupper((unsigned char)*in);
        if(c=='-'||isspace(c)) continue;
        const char*p=strchr(LIC_B32,c); if(!p||!c) return -1;
        v=(v<<5)|(unsigned)(p-LIC_B32); bits+=5;
        if(bits>=8){ if(o>=cap) return -1; out[o++]=(v>>(bits-8))&0xFF; bits-=8; }
    }
    return o;
}

/* MAC表記("00:1a:2b:..." 等)から装置ID(12桁大文字hex)を作る。成功=0 */
static inline int lic_normalize_mac(const char*in,char out[13]){
    int o=0;
    for(;*in;in++) if(isxdigit((unsigned char)*in)){ if(o>=12) return -1; out[o++]=(char)toupper((unsigned char)*in); }
    out[o]=0;
    return (o==12&&strcmp(out,"000000000000")!=0)?0:-1;
}

static inline int lic_message(const char*devid,int features,char*msg,int cap){
    return snprintf(msg,cap,"HDIPSRT1|%s|%02X",devid,features&0xFF);
}

/* キー検証。有効なら機能ビット(>0)、無効なら0。 */
static inline int lic_verify(const char*key,const char*devid){
    unsigned char blob[LIC_BLOB_LEN+4];
    if(!key||!devid) return 0;
    if(lic_b32_decode(key,blob,sizeof blob)!=LIC_BLOB_LEN) return 0;
    int features=blob[0];
    char msg[80]; int ml=lic_message(devid,features,msg,sizeof msg);
    EVP_PKEY*pk=EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519,NULL,LIC_PUBKEY,32);
    if(!pk) return 0;
    EVP_MD_CTX*ctx=EVP_MD_CTX_new(); int ok=0;
    if(ctx && EVP_DigestVerifyInit(ctx,NULL,NULL,NULL,pk)==1)
        ok = EVP_DigestVerify(ctx,blob+1,64,(const unsigned char*)msg,ml)==1;
    EVP_MD_CTX_free(ctx); EVP_PKEY_free(pk);
    return ok ? features : 0;
}


static inline void lic_describe(int features,char*out,int cap){
    snprintf(out,cap,"%s%s%s%s",
        (features&LIC_SRT_IP)?"SRT(IP接続) ":"",
        (features&LIC_SRT_NGN)?"SRT(NGN/CLEAR) ":"",
        (features&LIC_SRT_ENC)?"SRT暗号化 ":"",
        features?"":"なし");
}

#endif
