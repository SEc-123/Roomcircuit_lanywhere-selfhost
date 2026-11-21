package main

import (
	"context"
	"encoding/hex"
	"strings"
	"time"
)

const durableGrantPrefix = "grant:"
const admissionTTL = 3 * time.Minute

type gameAdmission struct {
	deviceID string
	grant    string
	expires  time.Time
}

// A durable reference can only come from the verified issuer and the device store.
func (a *adapter) revalidateGrant(ctx context.Context, saved string) (player, int) {
	id, durable := strings.CutPrefix(saved, durableGrantPrefix)
	if !durable {
		return a.checkGrant(ctx, saved)
	}
	value, err := hex.DecodeString(id)
	if err != nil || len(value) != 24 {
		return player{}, 403
	}
	who, status := a.grantRequest(ctx, "/internal/grants/check", map[string]string{"grant_id": id})
	if status == 200 && who.GrantID != id {
		return player{}, 503
	}
	return who, status
}

// Only an own game service that has already admitted a signed login may
// renew this lease without the now-expired public login token.
func (a *adapter) connectedPlayer(ctx context.Context, pid uint64) (player, int) {
	a.mu.Lock()
	current, exists := a.admitted[pid]
	item := a.devices[current.deviceID]
	a.mu.Unlock()
	if !exists {
		return player{}, 404
	}
	if !time.Now().Before(current.expires) || item.owner.PID != pid || item.owner.Application != a.application || current.grant == "" || item.grant != current.grant {
		return player{}, 403
	}
	who, status := a.revalidateGrant(ctx, current.grant)
	if status != 200 {
		if status == 401 || status == 404 {
			status = 403
		}
		return player{}, status
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	live := a.admitted[pid]
	item = a.devices[live.deviceID]
	now := time.Now()
	if live.deviceID != current.deviceID || live.grant != current.grant || !now.Before(live.expires) ||
		item.grant != live.grant || item.owner.Application != a.application ||
		who.PID != pid || item.owner.PID != who.PID || item.owner.Account != who.Account {
		return player{}, 403
	}
	live.expires = now.Add(admissionTTL)
	a.admitted[pid] = live
	who.User = item.owner.User
	return who, 200
}
