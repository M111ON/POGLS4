#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../pogls_v4_snapshot.h"

static int g_pass=0, g_fail=0;
#define section(s) printf("\n  -- %s\n",s)
#define check(c,ok,fail) do{if(c){printf("    v %s\n",ok);g_pass++;}else{printf("    x FAIL: %s\n",fail);g_fail++;}}while(0)

static DetachEntry make_e(uint64_t addr, uint8_t reason) {
    DetachEntry e; memset(&e,0,sizeof(e));
    e.angular_addr=addr; e.value=addr^0xDEAD;
    e.reason=reason; e.phase18=(uint8_t)(addr%18);
    return e;
}

static void t01_struct_sizes(void){
    section("T01  Struct sizes");
    check(sizeof(V4SnapshotHeader)>=60u,"Snapshot>=60B","wrong");
    check(sizeof(V4AuditTile)>=48u,"AuditTile>=48B","wrong");
}

static void t02_audit_init(void){
    section("T02  Audit init (satellite constellation)");
    V4AuditContext ctx; v4_audit_init(&ctx);
    check(ctx.magic==V4_AUDIT_MAGIC,"magic ok","wrong");
    check(ctx.health==AUDIT_HEALTH_OK,"health=OK","wrong");
    check(ctx.tile_count==54u,"54 tiles","wrong");
    /* verify satellite neighbors: ghost = lane+27 */
    check(ctx.tiles[0].neighbors[2]==27,"tile0 ghost→27","wrong");
    check(ctx.tiles[27].neighbors[2]==0,"tile27 ghost→0","wrong");
    check(ctx.tiles[10].neighbors[2]==37,"tile10 ghost→37","wrong");
}

static void t03_snap_create(void){
    section("T03  Snapshot create");
    V4SnapshotHeader s = v4_snap_create(1,100,0);
    check(s.snapshot_id==1,"snap_id=1","wrong");
    check(s.branch_id==100,"branch=100","wrong");
    check(s.state==SNAP_PENDING,"state=PENDING","wrong");
    check(s.magic==V4_SNAP_MAGIC,"magic ok","wrong");
}

static void t04_snap_certify(void){
    section("T04  Snapshot certify (happy path)");
    V4AuditContext ctx; v4_audit_init(&ctx);
    V4SnapshotHeader s = v4_snap_create(2,100,0);
    int r = v4_snap_certify(&s, 999, &ctx);
    check(r==0,"certify ok","wrong");
    check(s.state==SNAP_CONFIRMED_CERTIFIED,"state=CERTIFIED","wrong");
    check(s.certifier_id==999,"certifier=999","wrong");
    check(v4_snap_is_certified(&s),"is_certified=1","wrong");
    check(v4_snap_is_immune(&s),"is_immune=1","wrong");
    /* double certify rejected */
    check(v4_snap_certify(&s,999,&ctx)==-2,"double certify=-2","wrong");
}

static void t05_snap_auto_promote(void){
    section("T05  Auto promote + DEGRADED block");
    V4SnapshotHeader s = v4_snap_create(3,100,0);
    /* DEGRADED blocks auto-promote */
    check(v4_snap_auto_promote(&s,AUDIT_HEALTH_DEGRADED)==-3,"DEGRADED blocks","wrong");
    check(s.state==SNAP_PENDING,"still PENDING","wrong");
    /* OK allows */
    check(v4_snap_auto_promote(&s,AUDIT_HEALTH_OK)==0,"OK promotes","wrong");
    check(s.state==SNAP_CONFIRMED_AUTO,"state=AUTO","wrong");
}

static void t06_snap_void(void){
    section("T06  AUTO → VOID (one-shot)");
    V4SnapshotHeader s = v4_snap_create(4,100,0);
    v4_snap_auto_promote(&s,AUDIT_HEALTH_OK);
    check(v4_snap_invalidate(&s)==0,"invalidate ok","wrong");
    check(s.state==SNAP_VOID,"state=VOID","wrong");
    /* second invalidate rejected */
    check(v4_snap_invalidate(&s)==-2,"double void rejected","wrong");
    /* CERTIFIED cannot be voided */
    V4SnapshotHeader s2=v4_snap_create(5,100,0);
    v4_snap_certify(&s2,1,NULL);
    check(v4_snap_invalidate(&s2)==-2,"CERTIFIED not voidable","wrong");
}

static void t07_audit_ingest_clean(void){
    section("T07  Audit ingest clean events");
    V4AuditContext ctx; v4_audit_init(&ctx);
    /* no anomaly reason → tile goes CLEAN */
    DetachEntry e = make_e(100,0);
    v4_audit_ingest(&ctx,&e);
    check(ctx.total_scans==1,"scanned=1","wrong");
    uint8_t lane = (uint8_t)(100 % 54);
    check(ctx.tiles[lane].state==TILE_CLEAN,"tile CLEAN","wrong");
    check(ctx.tiles[lane].blocks_scanned==1,"blocks=1","wrong");
}

static void t08_audit_ingest_anomaly(void){
    section("T08  Audit ingest anomaly → signal queue");
    V4AuditContext ctx; v4_audit_init(&ctx);
    DetachEntry e = make_e(200, V4_ANOMALY_GEO_INVALID);
    v4_audit_ingest(&ctx,&e);
    check(ctx.total_anomalies==1,"anomalies=1","wrong");
    uint8_t lane=(uint8_t)(200%54);
    check(ctx.tiles[lane].state==TILE_ANOMALY,"tile ANOMALY","wrong");
    check(ctx.tiles[lane].anomaly_flags & V4_ANOMALY_GEO_INVALID,
          "flag set","wrong");
    /* signal queued for snapshot */
    check(ctx.sig_head==1,"signal queued","wrong");
}

static void t09_merkle(void){
    section("T09  Merkle root on certified snapshot");
    V4AuditContext ctx; v4_audit_init(&ctx);
    /* ingest clean events to build merkle */
    for(int i=0;i<10;i++){
        DetachEntry e=make_e((uint64_t)i*1000,0);
        v4_audit_ingest(&ctx,&e);
    }
    V4SnapshotHeader s=v4_snap_create(6,100,0);
    v4_snap_certify(&s,1,&ctx);
    check(s.merkle_root==ctx.merkle_root,"merkle matches","wrong");
    check(v4_verify_merkle(&s,&ctx),"merkle verify ok","wrong");
}

static void t10_3layer_verify(void){
    section("T10  3-layer verify");
    /* Layer 1: XOR audit */
    check(v4_verify_xor(0x1234567890ABCDEFULL,
                         ~0x1234567890ABCDEFULL),"XOR ok","wrong");
    check(!v4_verify_xor(0x1234,0x1234),"XOR fail ok","wrong");
    /* Layer 2: Fibo intersect */
    check(v4_verify_fibo(0xF0,0x0F,0xAA,0x55),"Fibo ok","wrong");
    check(!v4_verify_fibo(0xFF,0xFF,0xFF,0xFF),"Fibo fail ok","wrong");
    /* Layer 3: Merkle (null guard) */
    check(!v4_verify_merkle(NULL,NULL),"merkle(NULL)=0","crash");
}

static void t11_satellite_neighbors(void){
    section("T11  Satellite neighbor pattern (Iridium topology)");
    V4AuditContext ctx; v4_audit_init(&ctx);
    /* inject anomaly on tile 0 */
    DetachEntry e=make_e(0, V4_ANOMALY_GEO_INVALID);
    v4_audit_ingest(&ctx,&e);
    /* neighbors of tile 0: 1, 53, 27 */
    check(ctx.tiles[0].neighbors[0]==1,"nb0=1","wrong");
    check(ctx.tiles[0].neighbors[1]==53,"nb1=53","wrong");
    check(ctx.tiles[0].neighbors[2]==27,"nb2=27(ghost)","wrong");
    /* neighbors still clean → isolated anomaly */
    check(ctx.tiles[1].state==TILE_IDLE,"neighbor clean","wrong");
    check(ctx.tiles[27].state==TILE_IDLE,"ghost neighbor clean","wrong");
}

static void t12_health_degrade(void){
    section("T12  Health degrades on many anomalies");
    V4AuditContext ctx; v4_audit_init(&ctx);
    /* flood with anomalies > 25% */
    for(int i=0;i<50;i++){
        DetachEntry e=make_e((uint64_t)i*100, V4_ANOMALY_GEO_INVALID);
        v4_audit_ingest(&ctx,&e);
    }
    check(ctx.health==AUDIT_HEALTH_DEGRADED,"health DEGRADED","wrong");
    /* DEGRADED blocks auto-promote */
    V4SnapshotHeader s=v4_snap_create(7,100,0);
    check(v4_snap_auto_promote(&s,ctx.health)==-3,"promote blocked","wrong");
}

static void t13_wire_detach_to_audit(void){
    section("T13  Wire: DetachEntry batch → Audit → Snapshot");
    V4AuditContext ctx; v4_audit_init(&ctx);
    DetachEntry batch[8];
    for(int i=0;i<8;i++)
        batch[i]=make_e((uint64_t)(i*12345)%1048576, 0); /* clean */
    v4_audit_ingest_batch(&ctx,batch,8);
    check(ctx.total_scans==8,"batch: 8 scanned","wrong");
    /* certify snapshot after batch */
    V4SnapshotHeader s=v4_snap_create(8,100,0);
    check(v4_snap_certify(&s,42,&ctx)==0,"certify after batch","wrong");
    check(v4_snap_is_certified(&s),"certified","wrong");
}

static void t14_null_safety(void){
    section("T14  NULL safety");
    v4_audit_init(NULL);
    v4_audit_ingest(NULL,NULL);
    v4_audit_ingest_batch(NULL,NULL,0);
    v4_audit_stats(NULL);
    check(v4_snap_certify(NULL,0,NULL)==-1,"certify(NULL)=-1","crash");
    check(v4_snap_auto_promote(NULL,AUDIT_HEALTH_OK)==-1,"promote(NULL)=-1","crash");
    check(!v4_snap_is_certified(NULL),"certified(NULL)=0","crash");
    check(!v4_snap_is_immune(NULL),"immune(NULL)=0","crash");
    check(1,"all null paths ok","crash");
}

int main(void){
    printf("══════════════════════════════════════════════════\n");
    printf("  POGLS V4 Snapshot + Satellite Audit — Tests\n");
    printf("══════════════════════════════════════════════════\n");
    t01_struct_sizes(); t02_audit_init(); t03_snap_create();
    t04_snap_certify(); t05_snap_auto_promote(); t06_snap_void();
    t07_audit_ingest_clean(); t08_audit_ingest_anomaly();
    t09_merkle(); t10_3layer_verify(); t11_satellite_neighbors();
    t12_health_degrade(); t13_wire_detach_to_audit(); t14_null_safety();
    printf("\n══════════════════════════════════════════════════\n");
    if(g_fail==0) printf("  %d / %d PASS  v ALL PASS — Snapshot+Audit live [S]\n",g_pass,g_pass);
    else printf("  %d / %d PASS  x %d FAILED\n",g_pass,g_pass+g_fail,g_fail);
    printf("══════════════════════════════════════════════════\n");
    return g_fail>0?1:0;
}
