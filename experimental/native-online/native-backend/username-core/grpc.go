// SPDX-License-Identifier: AGPL-3.0-only
package main

import (
	"context"
	"encoding/hex"
	"errors"
	"github.com/jackc/pgx/v5"
	accountv1 "github.com/openpak/account-api/openpak/account/v1"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"time"
)

func (c *core) GetAccount(ctx context.Context, r *accountv1.GetAccountRequest) (*accountv1.GetAccountResponse, error) {
	if r.GetAccountId() == "roomcircuit-username-realm-v1" {
		return &accountv1.GetAccountResponse{AccountId: "roomcircuit-username-realm-v1", DisplayName: "roomcircuit-username-v1"}, nil
	}
	var name, state string
	e := c.db.QueryRow(ctx, `SELECT username,state FROM rc_user_account WHERE id=$1`, r.GetAccountId()).Scan(&name, &state)
	if errors.Is(e, pgx.ErrNoRows) {
		return nil, status.Error(codes.NotFound, "account not found")
	}
	if e != nil {
		return nil, status.Error(codes.Unavailable, "account unavailable")
	}
	st := accountv1.AccountStatus_ACCOUNT_STATUS_ACTIVE
	if state != "active" {
		st = accountv1.AccountStatus_ACCOUNT_STATUS_BANNED
	}
	return &accountv1.GetAccountResponse{AccountId: r.GetAccountId(), Status: st, Handle: name, DisplayName: name, Email: "", EmailVerified: false}, nil
}
func subjectOK(s string) bool { v, e := hex.DecodeString(s); return e == nil && len(v) == 32 }
func (c *core) ReserveLink(ctx context.Context, r *accountv1.ReserveLinkRequest) (*accountv1.ReserveLinkResponse, error) {
	if r.GetNamespace() != namespace || !subjectOK(r.GetSubjectId()) || len(r.GetAccountId()) != 36 {
		return nil, status.Error(codes.InvalidArgument, "invalid link")
	}
	tx, e := c.db.Begin(ctx)
	if e != nil {
		return nil, status.Error(codes.Unavailable, "link unavailable")
	}
	defer tx.Rollback(ctx)
	var state string
	e = tx.QueryRow(ctx, `SELECT state FROM rc_user_account WHERE id=$1`, r.GetAccountId()).Scan(&state)
	if errors.Is(e, pgx.ErrNoRows) {
		return nil, status.Error(codes.FailedPrecondition, "account unavailable")
	}
	if e != nil {
		return nil, status.Error(codes.Unavailable, "account unavailable")
	}
	if state != "active" {
		return nil, status.Error(codes.FailedPrecondition, "account unavailable")
	}
	_, e = tx.Exec(ctx, `INSERT INTO rc_user_link(id,account_id,namespace,subject,state) VALUES($1,$2,$3,$4,'pending') ON CONFLICT(namespace,subject) DO NOTHING`, id(), r.GetAccountId(), namespace, r.GetSubjectId())
	if e != nil {
		return nil, status.Error(codes.Unavailable, "link unavailable")
	}
	var link, owner string
	e = tx.QueryRow(ctx, `SELECT id,account_id,state FROM rc_user_link WHERE namespace=$1 AND subject=$2 FOR UPDATE`, namespace, r.GetSubjectId()).Scan(&link, &owner, &state)
	if e != nil {
		return nil, status.Error(codes.Unavailable, "link unavailable")
	}
	if owner != r.GetAccountId() {
		return nil, status.Error(codes.AlreadyExists, "subject owned")
	}
	if state == "revoked" {
		_, e = tx.Exec(ctx, `UPDATE rc_user_link SET state='pending' WHERE id=$1`, link)
		if e != nil {
			return nil, status.Error(codes.Unavailable, "link unavailable")
		}
		state = "pending"
	}
	if e = tx.Commit(ctx); e != nil {
		return nil, status.Error(codes.Unavailable, "link unavailable")
	}
	result := accountv1.LinkState_LINK_STATE_PENDING
	if state == "active" {
		result = accountv1.LinkState_LINK_STATE_ACTIVE
	}
	return &accountv1.ReserveLinkResponse{LinkId: link, State: result}, nil
}
func (c *core) ActivateLink(ctx context.Context, r *accountv1.ActivateLinkRequest) (*accountv1.ActivateLinkResponse, error) {
	var state string
	e := c.db.QueryRow(ctx, `UPDATE rc_user_link l SET state='active' FROM rc_user_account a WHERE l.id=$1 AND l.account_id=a.id AND a.state='active' AND l.state IN ('pending','active') RETURNING l.state`, r.GetLinkId()).Scan(&state)
	if errors.Is(e, pgx.ErrNoRows) {
		return nil, status.Error(codes.FailedPrecondition, "link unavailable")
	}
	if e != nil {
		return nil, status.Error(codes.Unavailable, "link unavailable")
	}
	return &accountv1.ActivateLinkResponse{State: accountv1.LinkState_LINK_STATE_ACTIVE}, nil
}
func (c *core) GetLinkBySubject(ctx context.Context, r *accountv1.GetLinkBySubjectRequest) (*accountv1.GetLinkBySubjectResponse, error) {
	var l, a, state string
	e := c.db.QueryRow(ctx, `SELECT id,account_id,state FROM rc_user_link WHERE namespace=$1 AND subject=$2`, r.GetNamespace(), r.GetSubjectId()).Scan(&l, &a, &state)
	if errors.Is(e, pgx.ErrNoRows) {
		return nil, status.Error(codes.NotFound, "link not found")
	}
	if e != nil {
		return nil, status.Error(codes.Unavailable, "link unavailable")
	}
	if state != "active" {
		return nil, status.Error(codes.NotFound, "link not active")
	}
	return &accountv1.GetLinkBySubjectResponse{LinkId: l, AccountId: a, State: accountv1.LinkState_LINK_STATE_ACTIVE, SubjectId: r.GetSubjectId()}, nil
}
func (c *core) UnlinkLink(ctx context.Context, r *accountv1.UnlinkLinkRequest) (*accountv1.UnlinkLinkResponse, error) {
	tx, e := c.db.Begin(ctx)
	if e != nil {
		return nil, status.Error(codes.Unavailable, "unlink unavailable")
	}
	defer tx.Rollback(ctx)
	// Serialize sequence assignment and commit so polling never skips an uncommitted event.
	if _, e = tx.Exec(ctx, `SELECT pg_advisory_xact_lock(597328165)`); e != nil {
		return nil, status.Error(codes.Unavailable, "unlink unavailable")
	}
	var owner, ns, subject, state string
	e = tx.QueryRow(ctx, `SELECT account_id,namespace,subject,state FROM rc_user_link WHERE id=$1 FOR UPDATE`, r.GetLinkId()).Scan(&owner, &ns, &subject, &state)
	if errors.Is(e, pgx.ErrNoRows) {
		return nil, status.Error(codes.NotFound, "link not found")
	}
	if e != nil {
		return nil, status.Error(codes.Unavailable, "unlink unavailable")
	}
	if state != "revoked" {
		if _, e = tx.Exec(ctx, `UPDATE rc_user_link SET state='revoked' WHERE id=$1`, r.GetLinkId()); e == nil {
			_, e = tx.Exec(ctx, `INSERT INTO rc_user_event(id,event_type,account_id,namespace,subject) VALUES($1,'link_unlinked',$2,$3,$4)`, id(), owner, ns, subject)
		}
		if e != nil {
			return nil, status.Error(codes.Unavailable, "unlink unavailable")
		}
	}
	if e = tx.Commit(ctx); e != nil {
		return nil, status.Error(codes.Unavailable, "unlink unavailable")
	}
	return &accountv1.UnlinkLinkResponse{State: accountv1.LinkState_LINK_STATE_REVOKED}, nil
}
func (c *core) PollEvents(ctx context.Context, r *accountv1.PollEventsRequest) (*accountv1.PollEventsResponse, error) {
	limit := r.GetLimit()
	if limit <= 0 || limit > 1000 {
		limit = 1000
	}
	rows, e := c.db.Query(ctx, `SELECT version,id,event_type,account_id,namespace,subject,created_at FROM rc_user_event WHERE version>$1 ORDER BY version LIMIT $2`, r.GetSinceVersion(), limit)
	if e != nil {
		return nil, status.Error(codes.Unavailable, "events unavailable")
	}
	defer rows.Close()
	out := &accountv1.PollEventsResponse{MaxVersion: r.GetSinceVersion()}
	for rows.Next() {
		v := &accountv1.InvalidationEvent{}
		var at time.Time
		if e = rows.Scan(&v.Version, &v.EventId, &v.Type, &v.AccountId, &v.Namespace, &v.SubjectId, &at); e != nil {
			return nil, status.Error(codes.Unavailable, "events unavailable")
		}
		v.CreatedAtUnix = at.Unix()
		out.Events = append(out.Events, v)
		out.MaxVersion = v.Version
	}
	if rows.Err() != nil {
		return nil, status.Error(codes.Unavailable, "events unavailable")
	}
	return out, nil
}
