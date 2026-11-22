// SPDX-License-Identifier: AGPL-3.0-only
// Test-only driver for a disposable account core and its real event outbox.
package main

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net"
	"os"
	"time"

	accountv1 "github.com/openpak/account-api/openpak/account/v1"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/metadata"
)

const namespace = "roomcircuit-lab"

func main() {
	version, err := unlinkAndReactivate()
	if err != nil {
		log.Fatal(err)
	}
	if err := json.NewEncoder(os.Stdout).Encode(map[string]uint64{"unlink_version": version}); err != nil {
		log.Fatal("cannot write event verification receipt")
	}
}

func unlinkAndReactivate() (uint64, error) {
	address, key := os.Getenv("RC_TEST_CORE_ADDRESS"), os.Getenv("RC_TEST_CORE_KEY")
	host, _, err := net.SplitHostPort(address)
	if err != nil || host != "127.0.0.1" || len(key) < 32 {
		return 0, errors.New("a disposable loopback core and private key are required")
	}
	var input struct {
		Account string `json:"account"`
		Subject string `json:"subject"`
	}
	decoder := json.NewDecoder(io.LimitReader(os.Stdin, 1024))
	if decoder.Decode(&input) != nil || decoder.Decode(new(any)) != io.EOF || input.Account == "" {
		return 0, errors.New("invalid test-only account link input")
	}
	if subject, err := hex.DecodeString(input.Subject); err != nil || len(subject) != 32 {
		return 0, errors.New("invalid test-only account subject")
	}
	var nonce [16]byte
	if _, err := rand.Read(nonce[:]); err != nil {
		return 0, errors.New("test idempotency key unavailable")
	}
	action := "synthetic-core-test:" + hex.EncodeToString(nonce[:])
	connection, err := grpc.NewClient(address, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return 0, errors.New("cannot reach the disposable account core")
	}
	defer connection.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 12*time.Second)
	defer cancel()
	ctx = metadata.AppendToOutgoingContext(ctx, "x-internal-key", key)
	links, events := accountv1.NewLinksClient(connection), accountv1.NewEventsClient(connection)
	lookup := &accountv1.GetLinkBySubjectRequest{Namespace: namespace, SubjectId: input.Subject}
	link, err := links.GetLinkBySubject(ctx, lookup)
	if err != nil || link.GetAccountId() != input.Account || link.GetState() != accountv1.LinkState_LINK_STATE_ACTIVE {
		return 0, errors.New("synthetic account does not own the expected active link")
	}
	before, err := events.PollEvents(ctx, &accountv1.PollEventsRequest{Limit: 1000})
	if err != nil || len(before.GetEvents()) >= 1000 {
		return 0, errors.New("cannot read the disposable event baseline")
	}
	unlinked, err := links.UnlinkLink(ctx, &accountv1.UnlinkLinkRequest{LinkId: link.GetLinkId(), IdempotencyKey: action + ":unlink"})
	if err != nil || unlinked.GetState() != accountv1.LinkState_LINK_STATE_REVOKED {
		return 0, errors.New("core did not revoke the synthetic link")
	}
	reserved, err := links.ReserveLink(ctx, &accountv1.ReserveLinkRequest{
		AccountId: input.Account, Namespace: namespace, SubjectId: input.Subject, IdempotencyKey: action + ":reserve",
	})
	if err != nil || reserved.GetLinkId() != link.GetLinkId() || reserved.GetState() != accountv1.LinkState_LINK_STATE_PENDING {
		return 0, errors.New("core did not reserve the same synthetic link row again")
	}
	activated, err := links.ActivateLink(ctx, &accountv1.ActivateLinkRequest{LinkId: link.GetLinkId(), IdempotencyKey: action + ":activate"})
	if err != nil || activated.GetState() != accountv1.LinkState_LINK_STATE_ACTIVE {
		return 0, errors.New("core did not reactivate the same synthetic link")
	}
	after, err := events.PollEvents(ctx, &accountv1.PollEventsRequest{SinceVersion: before.GetMaxVersion(), Limit: 100})
	if err != nil {
		return 0, errors.New("cannot query committed unlink events")
	}
	for _, event := range after.GetEvents() {
		if event.GetType() == "link_unlinked" && event.GetNamespace() == namespace && event.GetSubjectId() == input.Subject &&
			event.GetAccountId() == input.Account && event.GetVersion() > before.GetMaxVersion() {
			return event.GetVersion(), nil
		}
	}
	return 0, errors.New("same-link reactivation did not leave a committed account-core unlink event")
}
