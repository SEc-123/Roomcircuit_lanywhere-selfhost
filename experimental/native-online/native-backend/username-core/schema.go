package main

const schema = `
CREATE TABLE IF NOT EXISTS rc_user_account (id text PRIMARY KEY, username text NOT NULL UNIQUE, password_hash text NOT NULL, state text NOT NULL DEFAULT 'active', created_at timestamptz NOT NULL DEFAULT now());
CREATE TABLE IF NOT EXISTS rc_user_session (token_hash bytea PRIMARY KEY, account_id text NOT NULL REFERENCES rc_user_account(id), expires timestamptz NOT NULL);
CREATE INDEX IF NOT EXISTS rc_user_session_expiry ON rc_user_session(expires);
CREATE TABLE IF NOT EXISTS rc_user_rate (key bytea PRIMARY KEY, window_at timestamptz NOT NULL, count integer NOT NULL);
CREATE TABLE IF NOT EXISTS rc_user_link (id text PRIMARY KEY, account_id text NOT NULL REFERENCES rc_user_account(id), namespace text NOT NULL, subject text NOT NULL, state text NOT NULL, UNIQUE(namespace,subject));
CREATE TABLE IF NOT EXISTS rc_user_event (version bigserial PRIMARY KEY, id text NOT NULL, event_type text NOT NULL, account_id text NOT NULL, namespace text NOT NULL, subject text NOT NULL, created_at timestamptz NOT NULL DEFAULT now());
`
