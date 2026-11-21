'use strict';
const assert=require('node:assert/strict');
const {test}=require('node:test');
const directory=require('./node-directory');
const locator=require('./node-locator');
const catalog=require('./node-catalog');
const {create:friends,STATUS}=require('./node-friends');

function summary(code,game=0x100n){const row=Buffer.alloc(336);row.write(code);row.writeBigUInt64LE(game,8);row[32]=1;row[33]=4;return row;}
function query(offset=0,snapshot=0,game=0n,mask=0x1fff){const data=Buffer.alloc(20);data.writeUInt32LE(offset);data.writeUInt32LE(snapshot,4);data.writeBigUInt64LE(game,8);data.writeUInt32LE(mask,16);return data;}
function request(op,code,token,samples=[]){const out=Buffer.alloc(240);out.writeUInt32LE(op);if(code)out.write(code,4);token.copy(out,12);out.writeUInt32LE(samples.length,28);samples.forEach((sample,i)=>{const at=32+i*16;out.writeUInt32LE(sample.id,at);out.writeUInt16LE(sample.median||0,at+4);out.writeUInt16LE(sample.jitter||0,at+6);out[at+8]=sample.sent??5;out[at+9]=sample.received??5;out[at+10]=sample.kind??(catalog.nodes[sample.id].protocol===2?2:1);out[at+11]=sample.status??1;out.writeUInt16LE(sample.age||0,at+12);});return out;}

test('node rows carry actual upstream identity, preserve gateway fallback, filters and scoped stable pages',()=>{
 let time=0;const info=Buffer.alloc(0x480);info.writeBigUInt64LE(0x200n);info[0x10]=7;info[0x20]=2;
 const raw=locator.create(info,0x0a0d1234,2,'ABC234');assert(locator.valid(raw,0x200n,'ABC234',true));
 const sources=new Map([['self',[{summary:summary('BCD234')}]],['lanywhere-public',[{summary:summary('CDE234')}]],
  ['lanplay-nl',[{summary:summary('ABC234',0x200n),locator:raw},{summary:summary('DEF234',0x201n),locator:null}]]]);
 const service=directory.create({source:()=>sources,providerIds:['lanywhere-public','lanplay-nl'],communityDirect:true,now:()=>time});
 const status=service.status();assert(status.subarray(0,32).equals(catalog.hash));assert.equal(status.readUInt32LE(32),7);assert.equal(status.readUInt32LE(36),7);
 let page=service.page(query());assert.equal(page[8],4);assert.equal(page.length,16+4*408);assert.equal(page[9],3);assert.equal(page[10],13);
 let rows=Array.from({length:page[8]},(_,i)=>page.subarray(16+i*408,16+(i+1)*408));
 const actual=rows.find(row=>row.toString('ascii',0,6)==='ABC234');assert.equal(actual.readUInt32LE(336),2);assert.equal(actual.readUInt32LE(340),1);assert(actual.subarray(344).equals(raw));
 assert.equal(rows.find(row=>row.toString('ascii',0,6)==='DEF234').readUInt32LE(340),2);
 const filtered=service.page(query(0,0,0x200n,1<<2));assert.equal(filtered[8],1);assert.equal(filtered.readUInt32LE(16+336),2);
 assert.equal(service.page(query(0,filtered.readUInt32LE(),0x201n,1<<2))[11],1);
 assert.equal(service.page(query(0,0,0n,0))[8],0,'empty selected region stays empty');
 assert.equal(service.page(query(0,0,0n,1<<30)),null);
 sources.get('self').push(...['EFG234','FGH234','GHJ234','HJK234','JKL234','KLM234'].map(code=>({summary:summary(code)})));
 page=service.page(query());const generation=page.readUInt32LE(),next=page.readUInt32LE(4);assert.equal(next,6);
 sources.delete('lanplay-nl');const nextPage=service.page(query(next,generation));assert.equal(nextPage[8],4,'existing page generation retains original rows');
 time=30001;assert.equal(service.page(query(next,generation))[11],1);service.shutdown();
});

test('friend sessions are idempotent, anonymous, expire and recommend only jointly reachable measured nodes',()=>{
 let time=10000;const service=friends({now:()=>time});
 const a=Buffer.alloc(16,1),b=Buffer.alloc(16,2);
 const created=service.exchange(request(1,'',a),'same-network');assert.equal(created.readUInt32LE(),STATUS.OK);const code=created.toString('ascii',4,10);
 assert.equal(service.exchange(request(1,'',a),'same-network').toString('ascii',4,10),code);assert.equal(service.snapshot().sessions,1);
 assert.equal(service.exchange(request(2,code,b),'same-network')[30],2);
 const mine=[{id:0,median:40},{id:1,median:30},{id:2,median:20},{id:4,median:10}];
 let result=service.exchange(request(3,code,a,mine),'same-network');assert.equal(result[31],1);assert.equal(result[32+17],0);
 result=service.exchange(request(3,code,b,[{id:0,median:80},{id:1,median:100},{id:2,median:35},{id:4,median:30,received:3}]),'other-network');
 assert.equal(result[31],2);assert.equal(result[32+2*20+17],1);assert.equal(result[32+17],2);assert.equal(result[32+20+17],3);
 assert.equal(result[32+4*20+17],0,'poor success rate cannot be recommended');assert.equal(result.readUInt32LE(32+4),80,'worst participant median');
 result=service.exchange(request(3,code,b,[]),'other-network');assert.equal(result[31],1);assert.equal(result[32+17],0);
 assert.equal(service.exchange(request(4,code,Buffer.alloc(16,3)),'other').readUInt32LE(),STATUS.EXPIRED);
 const malformed=request(3,code,a,[{id:0,kind:2}]);assert.equal(service.exchange(malformed,'same-network').readUInt32LE(),STATUS.INVALID);
 time+=60001;result=service.exchange(request(4,code,a),'same-network');assert.equal(result[31],0);assert.equal(result[32+17],0);
 assert.equal(service.exchange(request(5,code,a),'same-network')[30],1);
 assert.equal(service.exchange(request(5,code,b),'other-network')[30],0);assert.equal(service.snapshot().sessions,0);
 const newSession=service.exchange(request(1,'',a),'same-network');const newCode=newSession.toString('ascii',4,10);time+=600001;
 assert.equal(service.exchange(request(4,newCode,a),'same-network').readUInt32LE(),STATUS.EXPIRED);service.shutdown();
});

test('friend participant capacity, node validation and bounded polling allow eight people sharing one network',()=>{
 const service=friends();const first=Buffer.alloc(16,1);const response=service.exchange(request(1,'',first),'shared');const code=response.toString('ascii',4,10);
 for(let i=2;i<=8;i++)assert.equal(service.exchange(request(2,code,Buffer.alloc(16,i)),'shared').readUInt32LE(),STATUS.OK);
 assert.equal(service.exchange(request(2,code,Buffer.alloc(16,9)),'shared').readUInt32LE(),STATUS.FULL);
 for(let second=0;second<30;second++)for(let i=1;i<=8;i++)assert.equal(service.exchange(request(4,code,Buffer.alloc(16,i)),'shared').readUInt32LE(),STATUS.OK);
 assert.equal(service.snapshot().participants,8);
 const duplicate=request(3,code,first,[{id:0,median:20},{id:0,median:25}]);assert.equal(service.exchange(duplicate,'shared').readUInt32LE(),STATUS.INVALID);
 const future=request(3,code,first,[{id:0,age:61}]);assert.equal(service.exchange(future,'shared').readUInt32LE(),STATUS.INVALID);
 service.shutdown();assert.deepEqual(service.snapshot(),{sessions:0,participants:0});
});

test('friend polling keeps active people while departed clients release capacity after ninety seconds',()=>{
 let time=0;const service=friends({now:()=>time});const host=Buffer.alloc(16,1),guest=Buffer.alloc(16,2);
 const opened=service.exchange(request(1,'',host),'host');const code=opened.toString('ascii',4,10);
 service.exchange(request(2,code,guest),'guest');
 time=60000;assert.equal(service.exchange(request(4,code,host),'host')[30],2);
 time=90001;const current=service.exchange(request(4,code,host),'host');assert.equal(current[30],1);
 assert.equal(service.exchange(request(4,code,guest),'guest').readUInt32LE(),STATUS.EXPIRED);
 assert.equal(service.exchange(request(2,code,guest),'guest')[30],2,'recovered guest can explicitly rejoin');
 time=180002;service.sweep();assert.deepEqual(service.snapshot(),{sessions:0,participants:0});
 service.shutdown();
});


test('group recommendations respect configured node availability',()=>{
 const service=friends({enabledMask:1}),token=Buffer.alloc(16,1);
 const opened=service.exchange(request(1,'',token),'host'),code=opened.toString('ascii',4,10);
 const result=service.exchange(request(3,code,token,[{id:0,median:100},{id:1,median:1}]),'host');
 assert.equal(result[32+17],1);assert.equal(result[32+20+17],0);
 service.shutdown();
});

test('direct room annotations require the preimage and an exact live locator, preserve legacy pages and never expose a hidden password', () => {
    const { createHash } = require('node:crypto');
    const { STATUS } = require('./direct-room-metadata');
    const proof = Buffer.alloc(32, 7), rawGame = 0xffffffffffffffffn;
    const hostIp = 0x0a0d1234, commitment = Buffer.alloc(8);
    commitment.writeUInt32LE(2);
    commitment.writeUInt32LE(hostIp, 4);
    const info = Buffer.alloc(0x480);
    info.writeBigUInt64LE(rawGame);
    createHash('sha256').update(proof).update(commitment).digest().copy(info, 16, 0, 16);
    Buffer.from('d00dabad2233', 'hex').copy(info, 32);
    const location = locator.create(info, hostIp, 1, 'ABC234');
    const item = { summary: summary('ABC234', rawGame), locator: location };
    const sources = new Map([[catalog.nodes[2].provider, []], [catalog.nodes[3].provider, [item]]]);
    let time = 0;
    const service = directory.create({
        source: () => sources, providerIds: [catalog.nodes[2].provider, catalog.nodes[3].provider],
        communityDirect: false, now: () => time,
    });
    const head = (op, node, bytes) => {
        const request = Buffer.alloc(bytes);
        request.writeUInt32LE(op);
        request.writeUInt32LE(node, 4);
        request.writeUInt32LE(hostIp, 8);
        location.copy(request, 12, 0, 38);
        return request;
    };
    const write = (mode = 0, pass = '', claim = proof) => {
        const request = head(1, 2, 384);
        request[50] = mode;
        request[51] = Buffer.byteLength(pass);
        request.write(pass, 52);
        claim.copy(request, 76);
        request.writeBigUInt64LE(0x0100000000001000n, 112);
        request.writeBigUInt64LE(0x0100000000001001n, 120);
        request.write('Test Game', 128, 'utf8');
        request.write('测试游戏', 256, 'utf8');
        return request;
    };
    const exchange = request => service.exchangeMetadata(request, 'fixture-source');
    const status = request => exchange(request).readUInt32LE();
    assert.equal(status(write(1, '1234')), STATUS.ROOM_UNAVAILABLE, 'password must not be staged before the room is seen');
    assert.equal(status(write()), STATUS.ROOM_UNAVAILABLE, 'valid title registration before discovery is pending');
    assert.equal(status(head(2, 2, 52)), STATUS.ROOM_UNAVAILABLE);
    sources.get(catalog.nodes[2].provider).push(item);
    const browse = query(0, 0, 0n, 1 << 2);
    const old = service.page(browse);
    assert.equal(old[8], 1);
    assert.equal(old[16 + 34], 0);
    assert.equal(old[16 + 35], 0);
    const annotated = service.page(browse, true), row = annotated.subarray(16);
    assert.equal(row.readBigUInt64LE(8), rawGame, 'legacy matching identity stays on wire');
    assert.equal(row.readBigUInt64LE(16), 0x0100000000001000n);
    assert.equal(row.toString('utf8', 80, 89), 'Test Game');
    assert.equal(row.toString('utf8', 208, 220), '测试游戏');
    assert.equal(row[34], 1);
    assert.equal(row.readUInt32LE(340), directory.DIRECT);
    assert.deepEqual(row.subarray(344), location);
    assert.equal(service.page(query(0, 0, 0n, 1 << 3), true)[16 + 34], 0, 'another node does not inherit the title');
    assert.equal(status(head(2, 3, 52)), STATUS.ROOM_UNAVAILABLE);
    assert.equal(status(write(1, 'test-pass', Buffer.alloc(32, 8))), STATUS.NOT_HOST);
    const accepted = exchange(write(1, 'test-pass'));
    assert.equal(accepted.readUInt32LE(), STATUS.OK);
    assert(accepted.subarray(4).every(byte => byte === 0), 'writes never echo secrets');
    let got = exchange(head(2, 2, 52));
    assert.equal(got.readUInt32LE(), STATUS.OK);
    assert.equal(got[4], 1);
    assert.equal(got.toString('ascii', 8, 8 + got[5]), 'test-pass');
    const oldSnapshot = query(0, old.readUInt32LE(), 0n, 1 << 2);
    assert.equal(service.page(oldSnapshot, true)[16 + 35], 1);
    assert.equal(service.page(oldSnapshot)[16 + 35], 0, 'old clients are isolated from new password flags');
    assert.equal(status(write(2, 'test-pass')), STATUS.INVALID);
    assert.equal(status(write(2)), STATUS.OK);
    got = exchange(head(2, 2, 52));
    assert.equal(got[4], 2);
    assert(got.subarray(5).every(byte => byte === 0), 'hidden mode never stores or returns password bytes');
    assert.equal(service.page(oldSnapshot, true)[16 + 35], 2, 'cached pages use current access state');
    const versioned = Buffer.concat([write(2), Buffer.alloc(16)]);
    versioned.writeBigUInt64LE(versioned.readBigUInt64LE(112), 120);
    versioned.write('v1.2.3', 384);
    assert.equal(status(versioned), STATUS.OK);
    const listedVersion = (request = oldSnapshot) => {
        const page = service.page(request, true, true);
        assert.equal(page.length, 16 + 424);
        return require('./room-game-version').parse(page.subarray(16 + 408), true);
    };
    assert.equal(listedVersion(), 'v1.2.3');
    assert.equal(listedVersion(query(0, 0, 0n, 1 << 3)), '');
    assert.equal(service.page(oldSnapshot, true).length, 16 + 408);
    versioned[399] = 'x'.charCodeAt(0);
    assert.equal(status(versioned), STATUS.INVALID, 'padded version cannot hide arbitrary bytes');
    const cloned = head(2, 2, 52);
    cloned[44] ^= 1;
    assert.equal(status(cloned), STATUS.ROOM_UNAVAILABLE);
    const wrongHost = head(2, 2, 52);
    wrongHost.writeUInt32LE(hostIp + 1, 8);
    assert.equal(status(wrongHost), STATUS.ROOM_UNAVAILABLE);
    const forgedNode = write();
    forgedNode.writeUInt32LE(3, 4);
    assert.equal(status(forgedNode), STATUS.NOT_HOST);
    const forgedAddress = write();
    forgedAddress.writeUInt32LE(hostIp + 1, 8);
    assert.equal(status(forgedAddress), STATUS.NOT_HOST);
    const duplicateLocation = locator.create(info, hostIp, 1, 'ABC235');
    sources.get(catalog.nodes[2].provider).push({ summary: summary('ABC235', rawGame), locator: duplicateLocation });
    assert.equal(status(head(2, 2, 52)), STATUS.ROOM_UNAVAILABLE, 'duplicate live identity fails closed');
    sources.get(catalog.nodes[2].provider).length = 0;
    assert.equal(service.page(oldSnapshot, true)[16 + 34], 0, 'persistent snapshot cannot attest a closed room');
    assert.equal(listedVersion(), '', 'version is erased when the source room is unavailable');
    assert.equal(status(head(2, 2, 52)), STATUS.ROOM_UNAVAILABLE);
    sources.get(catalog.nodes[2].provider).push(item);
    time = 45_001;
    assert.equal(service.page(browse, true)[16 + 34], 0);
    assert.equal(status(head(2, 2, 52)), STATUS.ROOM_UNAVAILABLE);
    service.shutdown();
});

test('valid pending direct room declarations have a per-source bound', () => {
    const { createHash } = require('node:crypto');
    const { STATUS } = require('./direct-room-metadata');
    const service = directory.create({
        source: () => new Map(), providerIds: [catalog.nodes[2].provider], communityDirect: false,
    });
    for (let i = 1; i <= 17; i++) {
        const body = Buffer.alloc(384);
        body.writeUInt32LE(1);
        body.writeUInt32LE(2, 4);
        body.writeUInt32LE(0x0a0d1234, 8);
        body.writeBigUInt64LE(10n, 12);
        body[44] = 6;
        const proof = Buffer.alloc(32, i);
        createHash('sha256').update(proof).update(body.subarray(4, 12)).digest().copy(body, 28, 0, 16);
        proof.copy(body, 76);
        assert.equal(service.exchangeMetadata(body, 'single-source').readUInt32LE(),
            i === 17 ? STATUS.BUSY : STATUS.ROOM_UNAVAILABLE);
    }
    service.shutdown();
});
