package main

import (
	"bytes"
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"maps"
	"time"

	accountv1 "github.com/openpak/account-api/openpak/account/v1"
	"google.golang.org/grpc/metadata"
)

const revocationPageSize = 512
const revocationPageBudget = 8
const maxRevocationTombstones = 32768

func decodeGrantFile(data []byte, dst any) error {
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if decoder.Decode(dst) != nil || decoder.Decode(new(any)) != io.EOF {
		return errors.New("invalid account grant state")
	}
	return nil
}

func (s *durableGrants) decode(data []byte) error {
	var stored persistedGrants
	if decodeGrantFile(data, &stored) == nil && stored.Format == 1 {
		if stored.Active == nil || stored.Revoked == nil || len(stored.Active) > maxDurableGrants || len(stored.Revoked) > maxRevocationTombstones {
			return errors.New("account grant store exceeds its bounds")
		}
		for key, version := range stored.Revoked {
			if key == "" || version == 0 || version > stored.Cursor {
				return errors.New("invalid account revocation cursor")
			}
		}
		s.active, s.cursor, s.revoked = stored.Active, stored.Cursor, stored.Revoked
		return nil
	}
	// Previous test brokers saved a bare map. Replay core history from zero
	// before using those grants since their original link generation is unknown.
	if decodeGrantFile(data, &s.active) != nil || s.active == nil || len(s.active) > maxDurableGrants {
		return errors.New("invalid previous account grant state")
	}
	return nil
}

func linkBindingKey(account, subject string) string {
	return account + ":" + subject
}

// Poll and commit with service.mu held, including the cursor and removals in
// one rename. Authorization retries if the bounded poll cannot catch up.
func (s *service) syncRevocations(ctx context.Context) error {
	ctx, cancel := context.WithTimeout(ctx, 2500*time.Millisecond)
	defer cancel()
	ctx = metadata.AppendToOutgoingContext(ctx, "x-internal-key", s.authority.key)
	for page := 0; page < revocationPageBudget; page++ {
		current := s.durable.cursor
		result, err := s.authority.events.PollEvents(ctx, &accountv1.PollEventsRequest{SinceVersion: current, Limit: revocationPageSize})
		if err != nil {
			return err
		}
		events := result.GetEvents()
		if len(events) == 0 {
			return nil
		}
		revoked := maps.Clone(s.durable.revoked)
		for _, event := range events {
			if event.GetVersion() <= current {
				return errors.New("account event order changed")
			}
			current = event.GetVersion()
			if event.GetType() == "link_unlinked" && event.GetNamespace() == ownLinkNamespace {
				device, subjectErr := hex.DecodeString(event.GetSubjectId())
				if len(event.GetAccountId()) != 36 || subjectErr != nil || len(device) != 32 {
					return errors.New("account unlink event omitted its binding")
				}
				revoked[linkBindingKey(event.GetAccountId(), event.GetSubjectId())] = current
			}
		}
		if current != result.GetMaxVersion() || len(events) > revocationPageSize || len(revoked) > maxRevocationTombstones {
			return errors.New("invalid account event batch")
		}
		active := maps.Clone(s.durable.active)
		for id, item := range active {
			if revoked[linkBindingKey(item.Account, item.Subject)] > item.LinkVersion {
				delete(active, id)
			}
		}
		if !s.durable.commitState(persistedGrants{Format: 1, Active: active, Cursor: current, Revoked: revoked}) {
			return errors.New("account revocation checkpoint could not be saved")
		}
		if len(events) < revocationPageSize {
			return nil
		}
	}
	return errors.New("account revocation history needs another bounded sync")
}
