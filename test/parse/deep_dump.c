/*
 * deep_dump.c — pcap 안의 모든 DEEP 메시지를 TSV 한 줄씩 덤프 (디버깅용)
 *
 * 출력(탭 구분, 헤더 1줄):
 *   seq        세션 내 계산된 메시지 시퀀스 번호 (first_seq + 패킷내 인덱스)
 *   pkt        pcap 프레임 번호 (1부터)
 *   midx       패킷 안에서 몇 번째 메시지인지 (0부터)
 *   type       메시지 타입 바이트 (문자)
 *   name       타입 이름
 *   flags      Event Flags / 타입별 1바이트 필드 (0x..)
 *   ts_ns      메시지 타임스탬프 (ns since epoch, 없으면 빈칸)
 *   ts_utc     사람이 읽는 UTC (없으면 빈칸)
 *   symbol     심볼 (없으면 빈칸)
 *   size       Size/Paired 등 4바이트 수량 (없으면 빈칸)
 *   price      Price (고정소수점, 없으면 빈칸)
 *   extra      타입별 추가 필드
 *
 * Build: gcc -O2 -o deep_dump deep_dump.c -lpcap
 * Run:   ./deep_dump ../data/20180127_IEXTP1_DEEP1.0.pcap > ../out/deep_dump.tsv
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <pcap.h>

static uint16_t rd16(const uint8_t*p){return (uint16_t)(p[0]|(p[1]<<8));}
static uint32_t rd32(const uint8_t*p){
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static uint64_t rd64(const uint8_t*p){uint64_t v=0;for(int i=7;i>=0;--i)v=(v<<8)|p[i];return v;}

static void fmt_price(int64_t px,char*o,size_t n){
    int neg=px<0; uint64_t a=neg?(uint64_t)(-px):(uint64_t)px;
    snprintf(o,n,"%s%"PRIu64".%04"PRIu64,neg?"-":"",a/10000,a%10000);
}
static void fmt_ts(int64_t ns,char*o,size_t n){
    time_t s=(time_t)(ns/1000000000LL); long f=(long)(ns%1000000000LL);
    struct tm tm; gmtime_r(&s,&tm); char b[24];
    strftime(b,sizeof b,"%Y-%m-%d %H:%M:%S",&tm);
    snprintf(o,n,"%.19s.%09ld",b,f);
}
static void fmt_sym(const uint8_t*p,char*o){
    int len=8; while(len>0&&(p[len-1]==' '||p[len-1]==0))len--;
    memcpy(o,p,len); o[len]=0;
}
static const char* tname(uint8_t t){
    switch(t){
    case 'S':return "SystemEvent"; case 'D':return "SecDirectory";
    case 'H':return "TradingStatus"; case 'O':return "OpsHalt";
    case 'P':return "ShortSaleTest"; case 'I':return "RetailLiqInd";
    case 'E':return "SecurityEvent"; case '8':return "PLU_Buy";
    case '5':return "PLU_Sell"; case 'T':return "TradeReport";
    case 'X':return "OfficialPrice"; case 'B':return "TradeBreak";
    case 'A':return "AuctionInfo"; default:return "unknown";
    }
}

static uint64_t g_out=0;

/* 한 메시지를 TSV 한 줄로 */
static void dump_msg(int64_t seq,uint64_t pkt,int midx,const uint8_t*m,uint16_t len){
    uint8_t t=len>=1?m[0]:0;
    char ts[40]="", tsn[24]="", sym[16]="", pxs[32]="", sz[16]="", extra[64]="";
    uint8_t flags = len>=2 ? m[1] : 0;

    /* off2 8바이트 타임스탬프를 갖는 타입들 */
    int has_ts = (t=='8'||t=='5'||t=='T'||t=='B'||t=='X'||t=='S'||t=='D'||
                  t=='H'||t=='O'||t=='P'||t=='I'||t=='E'||t=='A') && len>=10;
    if(has_ts){ int64_t ns=(int64_t)rd64(m+2); fmt_ts(ns,ts,sizeof ts);
                snprintf(tsn,sizeof tsn,"%"PRId64,ns); }

    if((t=='8'||t=='5') && len>=30){          /* Price Level Update */
        fmt_sym(m+10,sym); snprintf(sz,sizeof sz,"%u",rd32(m+18));
        fmt_price((int64_t)rd64(m+22),pxs,sizeof pxs);
    } else if((t=='T'||t=='B') && len>=38){    /* Trade */
        fmt_sym(m+10,sym); snprintf(sz,sizeof sz,"%u",rd32(m+18));
        fmt_price((int64_t)rd64(m+22),pxs,sizeof pxs);
        snprintf(extra,sizeof extra,"tradeid=%"PRIu64,rd64(m+30));
    } else if(t=='X' && len>=26){              /* Official Price */
        fmt_sym(m+10,sym); fmt_price((int64_t)rd64(m+18),pxs,sizeof pxs);
        snprintf(extra,sizeof extra,"pricetype=%c",m[1]);
    } else if(t=='S' && len>=10){              /* System Event */
        snprintf(extra,sizeof extra,"event=%c",m[1]);
    } else if(t=='D' && len>=31){              /* Security Directory */
        fmt_sym(m+10,sym); snprintf(sz,sizeof sz,"%u",rd32(m+18)); /* round lot */
        fmt_price((int64_t)rd64(m+22),pxs,sizeof pxs);
        snprintf(extra,sizeof extra,"luld=%u",m[30]);
    } else if(t=='H' && len>=22){              /* Trading Status */
        fmt_sym(m+10,sym); char r[5]; memcpy(r,m+18,4); r[4]=0;
        snprintf(extra,sizeof extra,"status=%c reason=%s",m[1],r);
    } else if(t=='O' && len>=18){              /* Operational Halt */
        fmt_sym(m+10,sym); snprintf(extra,sizeof extra,"halt=%c",m[1]);
    } else if(t=='P' && len>=19){              /* Short Sale Price Test */
        fmt_sym(m+10,sym); snprintf(extra,sizeof extra,"test=%u detail=%c",m[1],m[18]?m[18]:' ');
    } else if(t=='I' && len>=18){              /* Retail Liquidity Indicator */
        fmt_sym(m+10,sym); snprintf(extra,sizeof extra,"ind=%c",m[1]);
    } else if(t=='E' && len>=18){              /* Security Event */
        fmt_sym(m+10,sym); snprintf(extra,sizeof extra,"event=%c",m[1]);
    } else if(t=='A' && len>=80){              /* Auction Information */
        fmt_sym(m+10,sym); snprintf(sz,sizeof sz,"%u",rd32(m+18)); /* paired */
        fmt_price((int64_t)rd64(m+22),pxs,sizeof pxs);             /* ref price */
        char ind[32]; fmt_price((int64_t)rd64(m+30),ind,sizeof ind);
        snprintf(extra,sizeof extra,"auc=%c indic=%s imb=%u side=%c",m[1],ind,rd32(m+38),m[42]);
    }

    /* 고정폭 정렬: 빈 칸은 "-" 로 채워 컬럼 자리 유지 (터미널에서 눈으로 보기 쉽게) */
    printf("%8"PRId64" %7"PRIu64" %4d  %c  %-14s 0x%02x %-19s %-29s %-8s %10s %12s  %s\n",
           seq,pkt,midx,t?t:'?',tname(t),flags,
           tsn[0]?tsn:"-", ts[0]?ts:"-", sym[0]?sym:"-",
           sz[0]?sz:"-", pxs[0]?pxs:"-", extra[0]?extra:"-");
    g_out++;
}

static uint64_t g_pkt=0;

static void iextp(const uint8_t*p,uint32_t len){
    if(len<40||rd16(p+2)!=0x8004) return;
    uint16_t plen=rd16(p+12), mc=rd16(p+14);
    int64_t first_seq=(int64_t)rd64(p+24);
    if(mc==0) return;                          /* heartbeat: 메시지 없음 */
    const uint8_t*q=p+40,*end=p+40+plen;
    for(uint16_t i=0;i<mc && q+2<=end;i++){
        uint16_t ml=rd16(q); q+=2; if(q+ml>end) break;
        dump_msg(first_seq+i,g_pkt,i,q,ml);
        q+=ml;
    }
}

static void frame(const uint8_t*f,uint32_t cl){
    if(cl<14)return; uint16_t eth=(uint16_t)((f[12]<<8)|f[13]);uint32_t off=14;
    while(eth==0x8100&&off+4<=cl){eth=(uint16_t)((f[off+2]<<8)|f[off+3]);off+=4;}
    if(eth!=0x0800||off+20>cl)return; const uint8_t*ip=f+off;
    if((ip[0]>>4)!=4)return; uint32_t ihl=(ip[0]&0x0f)*4;
    if(ihl<20||off+ihl>cl||ip[9]!=17)return; uint32_t uo=off+ihl;
    if(uo+8>cl)return; const uint8_t*u=f+uo; uint16_t ul=(uint16_t)((u[4]<<8)|u[5]);
    if(ul<8)return; uint32_t po=uo+8,pl=ul-8; if(po+pl>cl)pl=cl-po;
    iextp(f+po,pl);
}

int main(int argc,char**argv){
    const char*path=argc>1?argv[1]:"../data/20180127_IEXTP1_DEEP1.0.pcap";
    char eb[PCAP_ERRBUF_SIZE]; pcap_t*pc=pcap_open_offline(path,eb);
    if(!pc){fprintf(stderr,"open: %s\n",eb);return 1;}
    printf("%8s %7s %4s %2s  %-14s %-4s %-19s %-29s %-8s %10s %12s  %s\n",
           "seq","pkt","midx","ty","name","flag","ts_ns","ts_utc","symbol","size","price","extra");
    struct pcap_pkthdr*h; const u_char*d; int rc;
    while((rc=pcap_next_ex(pc,&h,&d))==1){ g_pkt++; frame((const uint8_t*)d,h->caplen); }
    pcap_close(pc);
    fprintf(stderr,"dumped %"PRIu64" messages from %"PRIu64" packets -> stdout\n",g_out,g_pkt);
    return 0;
}
