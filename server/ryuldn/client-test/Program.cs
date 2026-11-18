// SPDX-License-Identifier: GPL-2.0-only
// Replace only the proxy transport and BSD service interface. The tested socket,
// request and packet classes come from the caller's pinned emulator checkout.
using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;
using Ryujinx.HLE.HOS.Services.Ldn.UserServiceCreator.LdnRyu.Proxy;
using Ryujinx.HLE.HOS.Services.Ldn.UserServiceCreator.LdnRyu.Types;

namespace Ryujinx.HLE.HOS.Services.Sockets.Bsd.Impl { interface ISocketImpl {} }
namespace Ryujinx.HLE.HOS.Services.Sockets.Bsd.Proxy
{
    enum WsaError { WSAECONNREFUSED = 10061, WSAECONNRESET = 10054, WSAEMSGSIZE = 10040, WSAETIMEDOUT = 10060, WSAEWOULDBLOCK = 10035 }
}
namespace Ryujinx.HLE.HOS.Services.Ldn.UserServiceCreator.LdnRyu.Proxy
{
    class LdnProxy
    {
        public IPAddress LocalAddress { get; set; } = IPAddress.Parse("10.13.1.2");
        public WireTransport Wire;
        readonly List<LdnProxySocket> listeners = new();
        public LdnProxySocket[] Subscribers() { lock(listeners) return listeners.ToArray(); }
        public IPEndPoint LastAccept, LastClose;
        public ManualResetEventSlim Request = new();
        public bool IsBroadcast(uint ip) => ip == 0x0a0dffff;
        public ushort GetEphemeralPort(ProtocolType t) => 30000;
        public void ReturnEphemeralPort(ProtocolType t, ushort n) {}
        public void RegisterSocket(LdnProxySocket s) { lock(listeners) listeners.Add(s); }
        public void UnregisterSocket(LdnProxySocket s) { lock(listeners) listeners.Remove(s); }
        public void SignalConnected(IPEndPoint me, IPEndPoint other, ProtocolType t) { LastAccept = me; Wire?.Proxy(19,me,other,t); }
        public void RequestConnection(IPEndPoint me, IPEndPoint other, ProtocolType t) { Request.Set(); Wire?.Proxy(18,me,other,t); }
        public void EndConnection(IPEndPoint me, IPEndPoint other, ProtocolType t) { LastClose = other ?? throw new Exception("Missing remote endpoint"); Wire?.Proxy(21,me,other,t); }
        public int SendTo(ReadOnlySpan<byte> buf, SocketFlags f, IPEndPoint me, IPEndPoint other, ProtocolType t) { Wire?.Proxy(20,me,other,t,buf.ToArray()); return buf.Length; }
    }
}
class Program
{
    static void Check(bool value, string message) { if (!value) throw new Exception(message); }
    static ProxyInfo Route(ProtocolType t) => new() { SourceIpV4=0x0a0d0103, DestIpV4=0x0a0d0102, SourcePort=45000, DestPort=4000, Protocol=t };
    static ProxyDataPacket Data(ProtocolType t, byte[] bytes) => new() { Header=new() { Info=Route(t), DataLength=(uint)bytes.Length }, Data=bytes };
    static LdnProxySocket Make(LdnProxy p, ProtocolType t) => new(AddressFamily.InterNetwork, t==ProtocolType.Tcp ? SocketType.Stream : SocketType.Dgram, t, p) { Blocking=true };
    static async Task Main(string[] args)
    {
        var p = new LdnProxy();
        var udp = Make(p, ProtocolType.Udp);
        udp.Bind(new IPEndPoint(IPAddress.Any,4000));
        var remote = new IPEndPoint(IPAddress.Parse("10.13.1.3"),45000);
        var read = Task.Run(() => { byte[] b = new byte[8]; return udp.Receive(b); });
        await Task.Delay(100);
        udp.IncomingData(Data(ProtocolType.Udp,[1,2]));
        Check(await read.WaitAsync(TimeSpan.FromSeconds(2)) == 2, "UDP blocking read");
        udp.Connect(remote);
        Check(udp.Connected && !p.Request.IsSet && udp.Send(new byte[]{2})==1,"connected UDP");
        udp.Close();
        var tcp = Make(p, ProtocolType.Tcp);
        var open = Task.Run(() => tcp.Connect(remote));
        Check(p.Request.Wait(2000),"TCP request");
        tcp.HandleConnectResponse(new() { Info=Route(ProtocolType.Tcp) });
        await open.WaitAsync(TimeSpan.FromSeconds(2));
        Check(tcp.Connected,"TCP active open");
        tcp.Close();
        Check(remote.Equals(p.LastClose),"TCP close endpoint");
        var listener = Make(p,ProtocolType.Tcp);
        listener.Bind(new IPEndPoint(IPAddress.Any,4000));
        listener.Listen(4);
        var pending = Task.Run(() => (LdnProxySocket)listener.Accept());
        await Task.Delay(100);
        listener.HandleConnectRequest(new() { Info=Route(ProtocolType.Tcp) });
        var accepted = await pending.WaitAsync(TimeSpan.FromSeconds(2));
        Check(p.LastAccept.Equals(new IPEndPoint(p.LocalAddress,4000)), "preserved accepted port");
        Check(accepted.RemoteEndPoint.Equals(remote),"accepted peer");
        accepted.IncomingData(Data(ProtocolType.Tcp,[1,2,3,4]));
        byte[] peek = new byte[2], body=new byte[4];
        Check(accepted.Receive(peek,SocketFlags.Peek)==2, "peek");
        accepted.HandleDisconnect(new() { Info=Route(ProtocolType.Tcp) });
        Check(accepted.Receive(body)==4 && body[0]==1 && body[3]==4 && accepted.Receive(body)==0, "drain before EOF");
        if (args.Length > 0) await WireTransport.Test(int.Parse(args[0]));
        Console.WriteLine("Patched upstream socket: UDP wake and connected send, TCP connect, accept, peek, drain and EOF passed.");
    }
}
