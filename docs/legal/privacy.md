# Napple Online Privacy Policy

**Version:** 2026-11-03 · **Effective:** 2026-11-03 · Plain-English description of what Napple Online collects. Not legal advice; the project owner should have it reviewed before a public launch.

The Napple app works fully offline without an account. This policy covers only the optional Napple Online service (accounts, profiles, friends, cloud saves, matchmaking). Data controller: the Napple project owner; contact `legal@napple.lat` (placeholder, to be confirmed before launch).

## What we collect

| Data | Why | Notes |
|---|---|---|
| Email address | sign-in, verification codes, password reset, account notices | never shown to other players |
| Password | sign-in | stored only as an Argon2id hash |
| Nickname, bio, links, icon | your in-game identity | shown according to your visibility tier |
| Profile photo (optional) | your in-game identity | we keep **only a re-encoded 256x256 copy**; the original and its metadata (EXIF, location) are discarded |
| Friends, requests, blocks, recent players | friends features | |
| Presence (which game and mode you are in) and per-game stats | friends features | shown according to your tier |
| Devices (name, platform, public key) | device approval and cloud-save key sharing | you can revoke devices |
| Cloud saves | sync between your devices | **encrypted on your device; we store ciphertext and cannot read it** |
| Match log | reports, bans, abuse prevention | see below |
| IP address and request logs | security and debugging | transient, see below |
| Accepted terms version, age confirmation, timestamps | proof of consent | |

## The match log (including Private and Ghost)

For every online match we record the game, match id, time and participants **linked to the real account**, and keep it for **30 days**. This applies to all visibility tiers, **including Private and Ghost**: those tiers hide you from other players, not from the server. We use it only to handle reports, bans and abuse, and to answer lawful requests. Other players never see your account behind a Ghost alias.

## What we do not do

No advertising, no ad or analytics trackers, no cookies on napple.lat beyond what the static site needs (none today), no selling or renting of data, no profiling, no reading your saves, no messaging content (there is no chat).

## Who processes data for us

- **Amazon Web Services (AWS)**: hosting and storage, and **Amazon SES** for sending verification and reset emails. Data may be processed in the United States; AWS acts under its data processing terms and standard contractual clauses.
- Nobody else. We do not contact Nintendo.

## How long we keep it

- Account, profile, friends, devices, saves: until you delete them or the account.
- Match log: 30 days, then deleted automatically.
- IP addresses and server logs: short-lived (target 14 days or less).
- Email bounce/complaint suppression entries: kept so we do not mail an address that asked to stop.
- Backups: removed within about 35 days after deletion.
- Reports and the evidence needed for a ban: up to 12 months after the case closes.

## Your rights

Under the EU/UK GDPR (and similar laws) you can access, export, correct and delete your data, object to or restrict processing, and complain to your data protection authority. Do it yourself in the game's **Online tab**: view and edit your profile, export your data, and delete your account (this removes profile, photo, friends, devices and cloud saves). For anything else write to `legal@napple.lat`. We answer within 30 days.

Legal bases (GDPR art. 6): contract (running your account), legitimate interests (security, abuse prevention, the 30-day match log) and consent where we ask for it (the photo, optional links).

## Children

Napple Online is for people 13 and over (or the higher age required locally). We delete accounts of younger users when we find them.

## Security

Passwords are hashed with Argon2id, sessions use short-lived tokens, cloud saves are end-to-end encrypted, traffic uses HTTPS. No system is perfectly secure; we will tell affected users and the authorities about serious breaches as required by law.

## Changes

We will update the version above when this policy changes, and tell you about material changes in the app.
