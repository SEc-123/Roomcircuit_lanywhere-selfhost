package main

import (
	"context"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
	"strings"
	"testing"
)

func TestInternalRequiresExactKey(t *testing.T) {
	key := strings.Repeat("k", 32)
	handler := func(context.Context, any) (any, error) { return "ok", nil }
	gate := internal(key)
	for _, input := range []struct {
		key  string
		want codes.Code
	}{{"", codes.Unauthenticated}, {"wrong", codes.Unauthenticated}, {key, codes.OK}} {
		ctx := metadata.NewIncomingContext(context.Background(), metadata.Pairs("x-internal-key", input.key))
		_, err := gate(ctx, nil, &grpc.UnaryServerInfo{}, handler)
		if status.Code(err) != input.want {
			t.Fatalf("unexpected status: %v", err)
		}
	}
}

func TestPasswordWorkIsBounded(t *testing.T) {
	c := &core{passwordSlots: make(chan struct{}, 4)}
	for i := 0; i < 4; i++ {
		if !c.acquirePassword() {
			t.Fatal("expected free slot")
		}
	}
	if c.acquirePassword() {
		t.Fatal("password work exceeded limit")
	}
	c.releasePassword()
	if !c.acquirePassword() {
		t.Fatal("released password slot not reusable")
	}
}
