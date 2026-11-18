// SPDX-License-Identifier: GPL-2.0-only
using System;
using System.Buffers.Binary;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Ryujinx.HLE.HOS.Services.Ldn.UserServiceCreator.LdnRyu.Types;
using Ryujinx.HLE.HOS.Services.Ldn.UserServiceCreator.LdnRyu.Proxy;

internal sealed class WireTransport : IDisposable
{
    readonly TcpClient client;
    readonly NetworkStream stream;
    readonly object writer = new();
    readonly LdnProxy proxy = new();
    readonly CancellationTokenSource done = new();

    WireTransport(int port)
    {
        client = new TcpClient();
        client.Connect(IPAddress.Loopback, port);
        stream = client.GetStream();
        stream.ReadTimeout = 4000;
        proxy.Wire = this;
        Send(0,new byte[22]);
        Read(0);
    }

    void Send(int kind, byte[] body)
    {
        byte[] header = new byte[12];
        "RLDN"u8.CopyTo(header);
        header[4] = (byte)kind;
        header[5] = 1;
        BinaryPrimitives.WriteInt32LittleEndian(header.AsSpan(8),body.Length);
        lock (writer) { stream.Write(header); stream.Write(body); }
    }

    (int kind,byte[] body) Read()
    {
        byte[] header = new byte[12]; stream.ReadExactly(header);
        if (!header.AsSpan(0,4).SequenceEqual("RLDN"u8)) throw new InvalidDataException("Header");
        int size = BinaryPrimitives.ReadInt32LittleEndian(header.AsSpan(8));
        if ((uint)size > 131059) throw new InvalidDataException("Frame size");
        byte[] data = new byte[size]; stream.ReadExactly(data);
        return (header[4],data);
    }

    byte[] Read(int expected)
    {
        var (kind,body)=Read();
        if (kind!=expected) throw new InvalidDataException($"Expected {expected}, received {kind}");
        return body;
    }

    byte[] Joined()
    {
        byte[] address=Read(17);
        uint ip=BinaryPrimitives.ReadUInt32LittleEndian(address);
        Span<byte> ipBytes=stackalloc byte[4]; BinaryPrimitives.WriteUInt32BigEndian(ipBytes, ip);
        proxy.LocalAddress=new IPAddress(ipBytes);
        return Read(15);
    }

    public void Dispose() { done.Cancel(); client.Dispose(); done.Dispose(); }

    static uint Ip(IPAddress a) => BinaryPrimitives.ReadUInt32BigEndian(a.GetAddressBytes());

    public void Proxy(int kind,IPEndPoint local,IPEndPoint remote,ProtocolType type,byte[] data=null)
    {
        byte[] body=new byte[(kind==20||kind==21 ? 20 : 16)+(data?.Length??0)];
        BinaryPrimitives.WriteUInt32LittleEndian(body,Ip(local.Address));
        BinaryPrimitives.WriteUInt16LittleEndian(body.AsSpan(4),(ushort)local.Port);
        BinaryPrimitives.WriteUInt32LittleEndian(body.AsSpan(6),Ip(remote.Address));
        BinaryPrimitives.WriteUInt16LittleEndian(body.AsSpan(10),(ushort)remote.Port);
        BinaryPrimitives.WriteInt32LittleEndian(body.AsSpan(12),(int)type);
        if (data != null) { BinaryPrimitives.WriteInt32LittleEndian(body.AsSpan(16),data.Length);data.CopyTo(body,20); }
        Send(kind,body);
    }

    void Pump()
    {
        try
        {
            while (!done.IsCancellationRequested)
            {
                var (kind,b)=Read();
                if (kind < 18 || kind > 21) continue;
                ProxyInfo info=new() { SourceIpV4=BinaryPrimitives.ReadUInt32LittleEndian(b),
                    SourcePort=BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan(4)),
                    DestIpV4=BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(6)),
                    DestPort=BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan(10)),
                    Protocol=(ProtocolType)BinaryPrimitives.ReadInt32LittleEndian(b.AsSpan(12)) };
                foreach(var s in proxy.Subscribers())
                {
                    if (s.ProtocolType!=info.Protocol || s.LocalEndPoint is not IPEndPoint ep || ep.Port!=info.DestPort) continue;
                    switch(kind) {
                        case 18: s.HandleConnectRequest(new() { Info=info }); break;
                        case 19: s.HandleConnectResponse(new() { Info=info }); break;
                        case 20: s.IncomingData(new() {Header=new() { Info=info,DataLength=(uint)(b.Length-20) },Data=b[20..]}); break;
                        case 21: s.HandleDisconnect(new() {Info=info}); break;
                    }
                }
            }
        }
        catch (Exception) when(done.IsCancellationRequested) { }
    }

    LdnProxySocket Socket(ProtocolType protocol,int port)
    {
        var s = new LdnProxySocket(AddressFamily.InterNetwork, protocol==ProtocolType.Udp ? SocketType.Dgram : SocketType.Stream, protocol, proxy) {Blocking=true};
        s.Bind(new IPEndPoint(IPAddress.Any,port)); return s;
    }

    public static async Task Test(int port)
    {
        using var host=new WireTransport(port);
        using var guest=new WireTransport(port);
        byte[] config=new byte[0xbc];
        Encoding.ASCII.GetBytes("SocketHost").CopyTo(config,0x44);
        BinaryPrimitives.WriteUInt64LittleEndian(config.AsSpan(0x74),0x0100152000022000);
        config[0x86]=4;config[0x88]=42;
        host.Send(2,config);
        var hosted=host.Joined();
        guest.Send(10,new byte[0x60]);
        var found=guest.Read(11);guest.Read(12);
        if (!found.AsSpan().SequenceEqual(hosted))throw new Exception("Wrong discovery");
        byte[] req=new byte[0x4fc];Encoding.ASCII.GetBytes("SocketJoin").CopyTo(req,0x44);
        req[0x74]=42;found.CopyTo(req,0x7c);guest.Send(13,req);
        var joined=guest.Joined();host.Read(7);
        if (joined[0x67]!=2)throw new Exception("Wrong participant count");
        _=Task.Run(host.Pump);_=Task.Run(guest.Pump);
        var timeout=TimeSpan.FromSeconds(4);
        var udpHost=host.Socket(ProtocolType.Udp,4200);
        var udpGuest=guest.Socket(ProtocolType.Udp,4201);
        var receive=Task.Run(()=>{byte[] buf=new byte[16];int n=udpGuest.Receive(buf);return Encoding.ASCII.GetString(buf,0,n);});
        udpHost.Connect(new IPEndPoint(guest.proxy.LocalAddress,4201));
        udpHost.Send("wire"u8);
        if (await receive.WaitAsync(timeout)!="wire")throw new Exception("UDP packet");
        var listener=host.Socket(ProtocolType.Tcp,4300);listener.Listen(4);
        var tcp=guest.Socket(ProtocolType.Tcp,4500);
        var accept=Task.Run(()=>(LdnProxySocket)listener.Accept());
        var connect=Task.Run(()=>tcp.Connect(new IPEndPoint(host.proxy.LocalAddress,4300)));
        var accepted=await accept.WaitAsync(timeout);await connect.WaitAsync(timeout);
        tcp.Send("match"u8);
        var data=await Task.Run(()=>{byte[] buf=new byte[16];int n=accepted.Receive(buf);return Encoding.ASCII.GetString(buf,0,n);}).WaitAsync(timeout);
        if (data!="match")throw new Exception("TCP up");
        accepted.Send("reply"u8);
        data=await Task.Run(()=>{byte[] buf=new byte[16];int n=tcp.Receive(buf);return Encoding.ASCII.GetString(buf,0,n);}).WaitAsync(timeout);
        if (data!="reply")throw new Exception("TCP down");
        tcp.Close(); if (await Task.Run(()=>accepted.Receive(new byte[1])).WaitAsync(timeout)!=0) throw new Exception("TCP EOF");
        Console.WriteLine("Two patched upstream sockets over real RyuLDN TCP into the RoomCircuit process: room scan, join, blocking UDP, TCP connect/accept, bidirectional bytes and EOF passed.");
    }
}
