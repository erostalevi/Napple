Stripe webhook events in the shape of the Stripe API (API version 2025-x "basil" moved `current_period_end` onto
subscription items; both places are present here so either API version parses). Authored from Stripe's published
event/object reference, not captured from a live account: no real ids, customers or keys. `{{ACCOUNT_ID}}`,
`"{{NOW}}"` and `"{{PERIOD_END}}"` are filled in by `tests/common/mod.rs::fixture`. The tests sign each delivery with
the fixture secret `whsec_test_napple_account_fixture` exactly as Stripe signs (`t=<ts>,v1=HMAC-SHA256(secret, "<ts>.<body>")`).
