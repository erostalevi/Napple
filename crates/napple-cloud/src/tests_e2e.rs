//! End to end against the in-process stub of the contract: several "devices" (namespaces / support dirs) on one machine.
use crate::session::{email_start, KeySetup, Session};
use crate::stub::{Options, Stub};
use crate::sync::{self, PullResult, PushResult, Target, DEFAULT_UID};
use crate::{savedir, store, Error};
use std::fs;
use std::path::{Path, PathBuf};

const TITLE: &str = "01006BB00C6F0000";
const TITLE2: &str = "0100000000010000";
const EMAIL: &str = "player@example.com";
const CHEAP: (u32, u32, u32) = (256, 1, 1);

fn tmp() -> PathBuf {
    let d = std::env::temp_dir().join(format!("napple-cloud-e2e-{}", std::process::id()));
    let _ = fs::remove_dir_all(&d);
    fs::create_dir_all(&d).unwrap();
    d
}

fn sign_in(stub: &Stub, support: &Path, ns: &str, name: &str) -> Session {
    email_start(&stub.base, EMAIL).unwrap();
    Session::email_finish(support, ns, &stub.base, EMAIL, "123456", name).unwrap()
}

/// Writes a committed save (snapshot + live + journal marker), as fs.c leaves it after nn::fs::CommitSaveData.
fn write_save(save_dir: &Path, name: &str, data: &[u8]) {
    let root = savedir::account_root(save_dir, DEFAULT_UID);
    let committed = PathBuf::from(format!("{}.committed", root.display()));
    for d in [&root, &committed] {
        fs::create_dir_all(d).unwrap();
        fs::write(d.join(name), data).unwrap();
    }
    fs::write(format!("{}.napple-journal", root.display()), b"").unwrap();
}

fn read_save(save_dir: &Path, name: &str) -> Option<Vec<u8>> {
    fs::read(savedir::account_root(save_dir, DEFAULT_UID).join(name)).ok()
}

fn target(s: &Session, title: &str, save_dir: &Path) -> Target {
    sync::targets(s, title, save_dir, &[DEFAULT_UID.to_string()]).pop().expect("default user linked")
}

#[test]
fn two_devices_end_to_end() {
    store::force_file_store(true);
    let stub = Stub::start("127.0.0.1:0", Options { fixed_code: Some("123456".into()), quiet: true, ..Default::default() }).unwrap();
    let root = tmp();
    let (support, saves_a, saves_b) = (root.join("support"), root.join("A/save"), root.join("B/save"));

    // device A: first device of the account -> makes the account save key and the recovery code
    let a = sign_in(&stub, &support, "devA", "Mac A");
    let phrase = match a.setup_keys(CHEAP).unwrap() {
        KeySetup::CreatedFirst { recovery_phrase } => recovery_phrase,
        _ => panic!("first device must create the key"),
    };
    let profile = a.link(DEFAULT_UID, None).unwrap();
    assert_eq!(a.profiles().unwrap().len(), 1);
    let ta = target(&a, TITLE, &saves_a);
    assert_eq!(sync::pull(&a, &ta, None).unwrap(), PullResult::NoRemote);
    write_save(&saves_a, "slot0.bin", b"PLAINTEXT-MARKER progress=1");
    assert_eq!(sync::pull(&a, &ta, None).unwrap(), PullResult::LocalNewer);
    assert_eq!(sync::push(&a, &ta, false).unwrap(), PushResult::Pushed { version: 1 });
    assert_eq!(sync::push(&a, &ta, false).unwrap(), PushResult::Unchanged);
    for b in stub.all_blobs() {
        assert!(!b.windows(16).any(|w| w == b"PLAINTEXT-MARKER"), "server sees ciphertext only");
        assert_eq!(&b[..4], b"NSAV");
    }

    // device B: same account, separate namespace (its own device key) -> needs approval from A
    let b = sign_in(&stub, &support, "devB", "iPhone B");
    assert!(matches!(b.setup_keys(CHEAP).unwrap(), KeySetup::WaitingApproval));
    assert!(matches!(b.account_key(), Err(Error::NoAccountKey)));
    let pending = a.key_requests().unwrap();
    assert_eq!(pending.len(), 1);
    assert_eq!(a.approve(None).unwrap(), vec![b.cfg().device_id.clone()]);
    assert!(matches!(b.setup_keys(CHEAP).unwrap(), KeySetup::Ready));
    assert_eq!(b.link(DEFAULT_UID, None).unwrap(), profile, "same profile on both devices");
    let tb = target(&b, TITLE, &saves_b);
    // pull before mount: B has no save yet -> it gets A's
    assert!(matches!(sync::pull(&b, &tb, None).unwrap(), PullResult::Pulled { version: 1, restore_point: None }));
    assert_eq!(read_save(&saves_b, "slot0.bin").unwrap(), b"PLAINTEXT-MARKER progress=1");
    assert_eq!(sync::pull(&b, &tb, None).unwrap(), PullResult::UpToDate);

    // B plays and pushes v2; A meanwhile also played offline from v1 -> conflict on A's push: both kept
    write_save(&saves_b, "slot0.bin", b"progress=2 (B)");
    assert_eq!(sync::push(&b, &tb, false).unwrap(), PushResult::Pushed { version: 2 });
    write_save(&saves_a, "slot0.bin", b"progress=2 (A)");
    let rp = match sync::push(&a, &ta, false).unwrap() {
        PushResult::Conflict { head: 2, restore_point } => restore_point,
        r => panic!("expected a conflict, got {r:?}"),
    };
    assert_eq!(a.dir.load_sync(TITLE, &profile).status, "conflict");
    // next launch on A: the remote becomes current, A's save stays as a restore point (the same one, deduplicated)
    match sync::pull(&a, &ta, None).unwrap() {
        PullResult::Pulled { version: 2, restore_point: Some(p) } => assert_eq!(p, rp),
        r => panic!("{r:?}"),
    }
    assert_eq!(read_save(&saves_a, "slot0.bin").unwrap(), b"progress=2 (B)");
    let kept = crate::state::StateDir::read_restore_point(&rp).unwrap();
    assert!(crate::tarball::parse(&kept).unwrap().iter().any(|e| e.data.as_deref() == Some(&b"progress=2 (A)"[..])));
    assert_eq!(a.dir.restore_points(TITLE, &profile).len(), 1);
    let ev: serde_json::Value = serde_json::from_slice(&fs::read(a.dir.root.join(format!("status/{TITLE}.json"))).unwrap()).unwrap();
    assert_eq!(ev["event"], "pulled");
    assert!(fs::read_to_string(a.dir.root.join("events.jsonl")).unwrap().contains("\"conflict\""));

    // B also edits without pushing, then pulls nothing new: local is newer, stays
    write_save(&saves_b, "slot0.bin", b"progress=3 (B)");
    assert_eq!(sync::pull(&b, &tb, None).unwrap(), PullResult::LocalNewer);
    assert_eq!(read_save(&saves_b, "slot0.bin").unwrap(), b"progress=3 (B)");

    // access token expiry -> refresh (rotated refresh token) transparently
    stub.expire_access_tokens();
    assert_eq!(sync::push(&b, &tb, false).unwrap(), PushResult::Pushed { version: 3 });

    // a second title syncs on its own (no plans this round: every title syncs)
    let ta2 = target(&a, TITLE2, &saves_a.parent().unwrap().join("save2"));
    write_save(&ta2.save_dir, "odyssey.bin", b"moons=5");
    assert_eq!(sync::push(&a, &ta2, false).unwrap(), PushResult::Pushed { version: 1 });
    assert_eq!(a.me().unwrap()["plan"]["name"], "free");

    // offline: the push is queued, the local save untouched; back online the queue drains
    let good = a.cfg().server;
    a.update_cfg(|c| c.server = "http://127.0.0.1:9/account/v1".into()).unwrap();
    let a_off = Session::open_ns(&support, "devA").unwrap();
    write_save(&saves_a, "slot0.bin", b"progress=4 (A offline)");
    assert!(matches!(sync::pull(&a_off, &ta, None), Err(Error::Net(_))));
    assert!(matches!(sync::push(&a_off, &ta, false).unwrap(), PushResult::Queued { .. }));
    assert_eq!(a_off.dir.load_queue().len(), 1);
    assert_eq!(read_save(&saves_a, "slot0.bin").unwrap(), b"progress=4 (A offline)");
    a_off.update_cfg(|c| c.server = good.clone()).unwrap();
    let a = Session::open_ns(&support, "devA").unwrap();
    // A is based on v2 but the cloud is at v3 (B): the queued push conflicts, A's offline save is kept as a restore point
    let r = sync::process_queue(&a);
    assert!(matches!(r[0].1, Ok(PushResult::Conflict { head: 3, .. })), "{:?}", r[0].1);
    assert!(a.dir.load_queue().is_empty());

    // history + restore a cloud version: becomes the new head everywhere
    let (vs, head) = sync::versions(&a, &ta).unwrap();
    assert_eq!((vs.len(), head), (3, 3));
    assert_eq!(sync::restore_version(&a, &ta, 1).unwrap(), PushResult::Pushed { version: 4 });
    assert_eq!(read_save(&saves_a, "slot0.bin").unwrap(), b"PLAINTEXT-MARKER progress=1");
    // B (which had pushed v3, local = v3) fast-forwards to the restored v4 without a conflict
    assert!(matches!(sync::pull(&b, &tb, None).unwrap(), PullResult::Pulled { version: 4, restore_point: None }));
    assert_eq!(read_save(&saves_b, "slot0.bin").unwrap(), b"PLAINTEXT-MARKER progress=1");
    // restore A's own restore point
    let pts = a.dir.restore_points(TITLE, &profile);
    let offline = pts.iter().find(|(p, _)| {
        let t = crate::state::StateDir::read_restore_point(p).unwrap();
        crate::tarball::parse(&t).unwrap().iter().any(|e| e.data.as_deref() == Some(&b"progress=4 (A offline)"[..]))
    });
    assert_eq!(sync::restore_point(&a, &ta, &offline.unwrap().0).unwrap(), PushResult::Pushed { version: 5 });

    // device C: unlocked with the recovery code instead of an approval
    let c = sign_in(&stub, &support, "devC", "Recovered Mac");
    assert!(matches!(c.setup_keys(CHEAP).unwrap(), KeySetup::WaitingApproval));
    assert!(c.recover("not a valid recovery code").is_err());
    c.recover(&phrase).unwrap();
    assert!(matches!(c.setup_keys(CHEAP).unwrap(), KeySetup::Ready));
    c.link(DEFAULT_UID, None).unwrap();
    let tc = target(&c, TITLE, &root.join("C/save"));
    assert!(matches!(sync::pull(&c, &tc, None).unwrap(), PullResult::Pulled { version: 5, .. }));
    assert_eq!(read_save(&tc.save_dir, "slot0.bin").unwrap(), b"progress=4 (A offline)");

    // per-title toggle
    c.update_cfg(|cfg| {
        cfg.titles.insert(TITLE.into(), crate::state::TitleCfg { enabled: false });
    })
    .unwrap();
    assert_eq!(sync::pull(&c, &tc, None).unwrap(), PullResult::Disabled);
    assert_eq!(sync::push(&c, &tc, false).unwrap(), PushResult::Disabled);

    // revoked device: refresh fails -> Unauthorized
    let c_id = c.cfg().device_id;
    a.call("DELETE", &format!("/devices/{c_id}"), None).unwrap();
    stub.expire_access_tokens();
    assert!(matches!(c.me(), Err(Error::Unauthorized)));

    // the runtime FFI on device D (namespace "default" in its own support dir): pull before mount, commit, flush on exit
    let support_d = root.join("D-support");
    let d = sign_in(&stub, &support_d, "default", "Mac D");
    assert!(matches!(d.setup_keys(CHEAP).unwrap(), KeySetup::WaitingApproval));
    a.approve(None).unwrap();
    d.link(DEFAULT_UID, None).unwrap();
    let saves_d = root.join("D/save");
    let cs = |p: &Path| std::ffi::CString::new(p.display().to_string()).unwrap();
    let on = unsafe {
        crate::ffi::napple_cloud_runtime_start(
            cs(&support_d).as_ptr(),
            cs(&saves_d).as_ptr(),
            c"01006bb00c6f0000".as_ptr(),
            std::ffi::CString::new(DEFAULT_UID).unwrap().as_ptr(),
            5000,
            None,
        )
    };
    assert_eq!(on, 1);
    assert_eq!(read_save(&saves_d, "slot0.bin").unwrap(), b"progress=4 (A offline)", "pulled before mount");
    write_save(&saves_d, "slot0.bin", b"progress=6 (D, via the runtime hook)");
    crate::ffi::napple_cloud_runtime_commit();
    assert_eq!(crate::ffi::napple_cloud_runtime_flush(10_000), 1);
    let st = unsafe { std::ffi::CStr::from_ptr(crate::ffi::napple_cloud_runtime_status()) }.to_str().unwrap().to_string();
    assert!(st.contains("\"version\":6"), "{st}");
    assert!(matches!(sync::pull(&b, &tb, None).unwrap(), PullResult::Pulled { version: 6, .. }));
    assert_eq!(read_save(&saves_b, "slot0.bin").unwrap(), b"progress=6 (D, via the runtime hook)");

    // sign-out keeps saves and restore points, removes secrets
    let a_dir = a.dir.clone();
    a.logout().unwrap();
    assert!(matches!(Session::open_ns(&support, "devA"), Err(Error::NotSignedIn)));
    assert!(!a_dir.restore_points(TITLE, &profile).is_empty());
    assert!(read_save(&saves_a, "slot0.bin").is_some());

    drop(stub);
    let _ = fs::remove_dir_all(root);
}

/// Signed out: the runtime entry point does nothing (no state written, returns 0).
#[test]
fn signed_out_is_a_no_op() {
    store::force_file_store(true);
    let root = tmp().join("signed-out");
    let save = root.join("save");
    let support = std::ffi::CString::new(root.display().to_string()).unwrap();
    let sd = std::ffi::CString::new(save.display().to_string()).unwrap();
    let r = unsafe {
        crate::ffi::napple_cloud_runtime_start(support.as_ptr(), sd.as_ptr(), c"01006BB00C6F0000".as_ptr(), c"x".as_ptr(), 100, None)
    };
    assert_eq!(r, 0);
    assert!(!root.join("cloud").exists());
    assert!(!save.exists());
}

/// 413 {reason: "quota"}: the save stays local, status `quota`, retried at most hourly unless forced.
#[test]
fn quota_keeps_the_save_local() {
    store::force_file_store(true);
    let stub = Stub::start("127.0.0.1:0", Options { fixed_code: Some("123456".into()), quiet: true, quota_bytes: Some(64), ..Default::default() }).unwrap();
    let root = tmp().join("quota");
    let support = root.join("support");
    let a = sign_in(&stub, &support, "q", "Mac Q");
    assert!(matches!(a.setup_keys(CHEAP).unwrap(), KeySetup::CreatedFirst { .. }));
    let profile = a.link(DEFAULT_UID, None).unwrap();
    let t = target(&a, TITLE, &root.join("save"));
    write_save(&t.save_dir, "big.bin", &[5u8; 4096]);
    assert!(matches!(sync::push(&a, &t, false).unwrap(), PushResult::Quota { .. }));
    let st = a.dir.load_sync(TITLE, &profile);
    assert_eq!(st.status, "quota");
    assert!(read_save(&t.save_dir, "big.bin").is_some());
    let n = stub.log().len();
    assert!(matches!(sync::push(&a, &t, false).unwrap(), PushResult::Quota { .. }));
    assert_eq!(stub.log().len(), n, "throttled: no request");
    assert!(matches!(sync::push(&a, &t, true).unwrap(), PushResult::Quota { .. }));
    assert!(stub.log().len() > n, "forced: asked again");
    assert!(fs::read_to_string(a.dir.root.join("events.jsonl")).unwrap().contains("\"quota\""));
}
