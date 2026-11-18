// SPDX-License-Identifier: GPL-2.0-only
using System;
using System.Net;
using System.Net.Sockets;
using Ryujinx.HLE.HOS.Services.Ldn.UserServiceCreator.LdnMitm;

class Program
{
    static void Check(bool pass)
    {
        if (!pass) throw new Exception("Unexpected Wi-Fi routing decision");
    }

    static void CheckWire(IPAddress subnetBroadcast)
    {
        using var gateway = new UdpClient(new IPEndPoint(RoomCircuitWifiRoute.Address, 0));
        gateway.Client.ReceiveTimeout = 2000;
        var listener = (IPEndPoint)gateway.Client.LocalEndPoint;
        var subnet = new IPEndPoint(subnetBroadcast, listener.Port);
        var limited = new IPEndPoint(IPAddress.Broadcast, listener.Port);
        var source = new IPEndPoint(IPAddress.Any, 0);
        using var mobile = new FixtureSocket();
        Check(!mobile.Socket.EnableBroadcast);
        var one = new byte[] { 1, 4, 9 };
        var two = new byte[] { 3, 5, 7 };
        Check(mobile.SendTo(out int sent, one, one.Length, BsdSocketFlags.None, limited) == LinuxError.SUCCESS);
        Check(sent == one.Length && gateway.Receive(ref source).AsSpan().SequenceEqual(one));
        Check(mobile.Connect(subnet) == LinuxError.SUCCESS);
        Check(mobile.RemoteEndPoint.Equals(subnet) && mobile.Socket.RemoteEndPoint.Equals(listener));
        Check(mobile.Socket.Send(two) == two.Length && gateway.Receive(ref source).AsSpan().SequenceEqual(two));
        mobile.Disconnect();
        Console.WriteLine("Patched MeloNX ManagedSocket methods: sendto and connected send reached reserved gateway over UDP with broadcast disabled");
    }

    static void Main(string[] args)
    {
        var phone = IPAddress.Parse("10.13.250.2");
        var subnetBroadcast = IPAddress.Parse("10.13.250.3");
        var local = new IPEndPoint(subnetBroadcast, 5172);
        var all = new IPEndPoint(IPAddress.Broadcast, 5173);
        var lan = new IPEndPoint(IPAddress.Parse("10.13.255.255"), 5174);
        var remote = new IPEndPoint(IPAddress.Parse("10.13.42.31"), 5175);
        var ipv6 = new IPEndPoint(IPAddress.IPv6Loopback, 5176);
        Check(RoomCircuitWifiRoute.ForInterface(false, phone, subnetBroadcast) == null);
        Check(RoomCircuitWifiRoute.ForInterface(true, IPAddress.Loopback, subnetBroadcast) == null);
        Check(RoomCircuitWifiRoute.ForInterface(true, IPAddress.IPv6Loopback, subnetBroadcast) == null);
        var host = RoomCircuitWifiRoute.ForInterface(true, phone, subnetBroadcast);
        var next = RoomCircuitWifiRoute.ForInterface(true, phone, subnetBroadcast);
        Check(RoomCircuitWifiRoute.Map(SocketType.Dgram, local) == local);
        host.Activate();
        foreach (var destination in new[] { local, all, lan })
        {
            var mapped = RoomCircuitWifiRoute.Map(SocketType.Dgram, destination);
            Check(mapped.Address.Equals(IPAddress.Parse("10.13.254.254")) && mapped.Port == destination.Port);
        }
        Check(RoomCircuitWifiRoute.Map(SocketType.Stream, local) == local);
        Check(RoomCircuitWifiRoute.Map(SocketType.Dgram, remote) == remote);
        Check(RoomCircuitWifiRoute.Map(SocketType.Dgram, ipv6) == ipv6);
        if (Array.IndexOf(args, "wire") >= 0) CheckWire(subnetBroadcast);
        next.Activate();
        host.Deactivate();
        Check(RoomCircuitWifiRoute.Map(SocketType.Dgram, local) != local);
        next.Deactivate();
        Check(RoomCircuitWifiRoute.Map(SocketType.Dgram, local) == local);
        Console.WriteLine("MeloNX C# UDP broadcast mapping: opt-in, /30, /16, limited broadcast, port, isolation and cleanup passed");
    }
}
