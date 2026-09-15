// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// stackchan-idf BLE 設定クライアント (動作確認用)。
//
// tools/settings.html の Web Bluetooth フローと同じアプリ層セッションを Rust で
// 実装する: KeyExchange で X25519 ハンドシェイク → HKDF-SHA256(salt=SHA256(password),
// info="stackchan-config-v1") → AES-256-GCM([12B nonce][ct][16B tag], AAD 無し)。
// 以後 Status を暗号 read、operation-mode 等を暗号 write、Apply で再起動できる。
//
// 例:
//   stackchan-ble scan
//   stackchan-ble status  --device Stackchan --password hogeFugapiyo
//   stackchan-ble set-mode 4 --device Stackchan --password hogeFugapiyo         # 書いて再起動
//   stackchan-ble set-mode 2 --device Stackchan --password hogeFugapiyo --no-apply
//   stackchan-ble read  e3f0a005-7b1c-4d2a-9e6f-2c5a8d4b1f00 --password ...     # 任意chrを暗号read
//   stackchan-ble write e3f0a025-7b1c-4d2a-9e6f-2c5a8d4b1f00 04 --password ...  # 任意chrを暗号write

use std::time::Duration;

use aes_gcm::aead::{Aead, KeyInit, Payload};
use aes_gcm::{Aes256Gcm, Key, Nonce};
use anyhow::{anyhow, bail, Context, Result};
use btleplug::api::{Central, Manager as _, Peripheral as _, ScanFilter, WriteType};
use btleplug::platform::{Manager, Peripheral};
use clap::{Parser, Subcommand};
use hkdf::Hkdf;
use rand::RngCore;
use sha2::{Digest, Sha256};
use uuid::Uuid;
use x25519_dalek::{EphemeralSecret, PublicKey};

// --- GATT UUIDs (settings_registry / gatt_settings.cpp と一致) ---------------
const SVC_UUID: Uuid = Uuid::from_u128(0xe3f0a000_7b1c_4d2a_9e6f_2c5a8d4b1f00);
const CHR_KX: Uuid = Uuid::from_u128(0xe3f0a006_7b1c_4d2a_9e6f_2c5a8d4b1f00);
const CHR_APPLY: Uuid = Uuid::from_u128(0xe3f0a004_7b1c_4d2a_9e6f_2c5a8d4b1f00);
const CHR_STATUS: Uuid = Uuid::from_u128(0xe3f0a005_7b1c_4d2a_9e6f_2c5a8d4b1f00);
const CHR_OPERATION_MODE: Uuid = Uuid::from_u128(0xe3f0a025_7b1c_4d2a_9e6f_2c5a8d4b1f00);
// OTA (gatt_settings.cpp / recovery/main/ble.cpp と一致)。
const CHR_OTA_CONTROL: Uuid = Uuid::from_u128(0xe3f0a009_7b1c_4d2a_9e6f_2c5a8d4b1f00);
const CHR_OTA_DATA: Uuid = Uuid::from_u128(0xe3f0a00a_7b1c_4d2a_9e6f_2c5a8d4b1f00);

const HKDF_INFO: &[u8] = b"stackchan-config-v1";
const NONCE_LEN: usize = 12;
const TAG_LEN: usize = 16;

#[derive(Parser)]
#[command(name = "stackchan-ble", about = "stackchan-idf BLE settings client (verification tool)")]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,

    /// デバイス名の部分一致 (advertising name)。省略時は "stackchan" を含むもの。
    #[arg(long, global = true, default_value = "stackchan")]
    device: String,

    /// 設定パスワード (auth-password)。未設定デバイスなら省略可。
    #[arg(long, global = true)]
    password: Option<String>,

    /// スキャン待ち時間 [秒]。
    #[arg(long, global = true, default_value_t = 8)]
    scan_secs: u64,
}

#[derive(Subcommand)]
enum Cmd {
    /// 近くの BLE デバイスを列挙 (Stackchan を探す)。
    Scan,
    /// ハンドシェイク後 Status(JSON) を暗号 read して表示。
    Status,
    /// operation-mode を書き込む (既定で Apply=再起動)。0..5。
    SetMode {
        mode: u8,
        /// Apply(再起動)しない — staging のみ。
        #[arg(long)]
        no_apply: bool,
    },
    /// 任意 characteristic を暗号 read (hex + UTF-8 表示)。
    Read { uuid: Uuid },
    /// 任意 characteristic を暗号 write (値は hex)。
    Write { uuid: Uuid, hex: String },
    /// ファームウェア (.bin) を OtaControl / OtaData 経由で書き込む。
    /// ADR-001 レイアウトの Main に対して実行すると Main は Recovery へ再起動する
    /// ("rebooting to recovery") ので、数秒待ってからもう一度実行する (Recovery が
    /// 同じ名前・同じ characteristic で受け付ける)。
    Ota {
        /// 書き込む .bin (build-<board>/stackchan_idf.bin)
        file: std::path::PathBuf,
        /// 1 チャンクの平文バイト数。暗号化で +28 B。ATT MTU 517 に収まる既定値。
        #[arg(long, default_value_t = 480)]
        chunk: usize,
        /// 何チャンクごとに OtaControl の状態を読んで進捗と整合を確認するか。
        #[arg(long, default_value_t = 32)]
        check_every: usize,
    },
    /// 機体自身に GitHub Pages からリリース (vX.Y.Z) を取得させる ({"op":"fetch"})。
    /// ADR-001 の Main は Recovery へ再起動して取得する (応答 "rebooting to recovery")。
    /// 進捗は 10 秒ほど待って Recovery に対して `status` を繰り返し読む。
    Fetch { tag: String },
}

#[tokio::main]
async fn main() -> Result<()> {
    let cli = Cli::parse();
    let manager = Manager::new().await.context("BLE manager")?;
    let adapter = manager
        .adapters()
        .await?
        .into_iter()
        .next()
        .ok_or_else(|| anyhow!("no Bluetooth adapter found"))?;

    if matches!(cli.cmd, Cmd::Scan) {
        return scan(&adapter, cli.scan_secs).await;
    }

    let peripheral = find_device(&adapter, &cli.device, cli.scan_secs).await?;
    peripheral.connect().await.context("connect")?;
    peripheral.discover_services().await.context("discover services")?;

    let result = run_session(&peripheral, &cli).await;
    let _ = peripheral.disconnect().await;
    result
}

async fn scan(adapter: &btleplug::platform::Adapter, secs: u64) -> Result<()> {
    adapter.start_scan(ScanFilter::default()).await?;
    tokio::time::sleep(Duration::from_secs(secs)).await;
    let peripherals = adapter.peripherals().await?;
    println!("found {} peripheral(s):", peripherals.len());
    for p in peripherals {
        let props = p.properties().await?.unwrap_or_default();
        let name = props.local_name.unwrap_or_else(|| "<no name>".into());
        println!("  {}  rssi={:?}  {}", p.address(), props.rssi, name);
    }
    let _ = adapter.stop_scan().await;
    Ok(())
}

async fn find_device(
    adapter: &btleplug::platform::Adapter,
    name_substr: &str,
    secs: u64,
) -> Result<Peripheral> {
    adapter.start_scan(ScanFilter::default()).await?;
    // --device が MAC アドレス形式なら address 一致、そうでなければ 名前部分一致 or
    // 我々の Service UUID を advertise しているものを採用 (名前非公開でも拾える)。
    let want_lower = name_substr.to_lowercase();
    let want_addr = name_substr.to_uppercase();
    let is_addr = name_substr.contains(':');
    let deadline = tokio::time::Instant::now() + Duration::from_secs(secs);
    loop {
        for p in adapter.peripherals().await? {
            let Some(props) = p.properties().await? else { continue };
            let name = props.local_name.clone().unwrap_or_default();
            let matched = if is_addr {
                p.address().to_string().to_uppercase() == want_addr
            } else {
                (!want_lower.is_empty() && name.to_lowercase().contains(&want_lower))
                    || props.services.contains(&SVC_UUID)
            };
            if matched {
                let shown = if name.is_empty() { "<no name>".into() } else { name };
                println!("connecting to {} ({})", shown, p.address());
                let _ = adapter.stop_scan().await;
                return Ok(p);
            }
        }
        if tokio::time::Instant::now() >= deadline {
            let _ = adapter.stop_scan().await;
            bail!("no device matching '{}' within {}s (try `scan`, or pass --device <MAC>)",
                  name_substr, secs);
        }
        tokio::time::sleep(Duration::from_millis(400)).await;
    }
}

/// アプリ層セッション: 鍵情報を保持し、暗号 read/write を提供する。
struct SecureSession {
    cipher: Aes256Gcm,
}

impl SecureSession {
    /// KeyExchange ハンドシェイクを実施してセッションを確立する。
    async fn establish(p: &Peripheral, password: Option<&str>) -> Result<Self> {
        let kx = find_chr(p, CHR_KX)?;
        let device_pub_raw = p.read(&kx).await.context("read KeyExchange (device pubkey)")?;
        if device_pub_raw.len() != 32 {
            bail!("device pubkey length {} != 32", device_pub_raw.len());
        }
        let mut dp = [0u8; 32];
        dp.copy_from_slice(&device_pub_raw);
        let device_pub = PublicKey::from(dp);

        let our_secret = EphemeralSecret::random_from_rng(rand::rngs::OsRng);
        let our_pub = PublicKey::from(&our_secret);
        let shared = our_secret.diffie_hellman(&device_pub);

        // HKDF-SHA256。salt = SHA-256(password)。password 無しは salt 無し (device 側
        // の no-auth デフォルトと一致)。
        let salt_vec: Option<[u8; 32]> =
            password.map(|pw| Sha256::digest(pw.as_bytes()).into());
        let hk = Hkdf::<Sha256>::new(salt_vec.as_ref().map(|s| s.as_slice()), shared.as_bytes());
        let mut key = [0u8; 32];
        hk.expand(HKDF_INFO, &mut key)
            .map_err(|_| anyhow!("HKDF expand failed"))?;
        let cipher = Aes256Gcm::new(Key::<Aes256Gcm>::from_slice(&key));

        // 自 pubkey を書いて device 側の complete_handshake を確定させる。
        p.write(&kx, our_pub.as_bytes(), WriteType::WithResponse)
            .await
            .context("write KeyExchange (our pubkey)")?;
        Ok(Self { cipher })
    }

    fn encrypt(&self, plain: &[u8]) -> Result<Vec<u8>> {
        let mut nonce = [0u8; NONCE_LEN];
        rand::rngs::OsRng.fill_bytes(&mut nonce);
        let ct = self
            .cipher
            .encrypt(Nonce::from_slice(&nonce), Payload { msg: plain, aad: &[] })
            .map_err(|_| anyhow!("AES-GCM encrypt failed"))?;
        let mut wire = Vec::with_capacity(NONCE_LEN + ct.len());
        wire.extend_from_slice(&nonce);
        wire.extend_from_slice(&ct); // aes-gcm returns ciphertext||tag
        Ok(wire)
    }

    fn decrypt(&self, wire: &[u8]) -> Result<Vec<u8>> {
        if wire.len() < NONCE_LEN + TAG_LEN {
            bail!("ciphertext too short ({} bytes)", wire.len());
        }
        let (nonce, ct_tag) = wire.split_at(NONCE_LEN);
        self.cipher
            .decrypt(Nonce::from_slice(nonce), Payload { msg: ct_tag, aad: &[] })
            .map_err(|_| anyhow!("AES-GCM decrypt/auth failed (wrong password?)"))
    }

    async fn read_chr(&self, p: &Peripheral, uuid: Uuid) -> Result<Vec<u8>> {
        let chr = find_chr(p, uuid)?;
        let wire = p.read(&chr).await.with_context(|| format!("read {}", uuid))?;
        self.decrypt(&wire)
    }

    async fn write_chr(&self, p: &Peripheral, uuid: Uuid, plain: &[u8]) -> Result<()> {
        let chr = find_chr(p, uuid)?;
        let wire = self.encrypt(plain)?;
        p.write(&chr, &wire, WriteType::WithResponse)
            .await
            .with_context(|| format!("write {}", uuid))
    }
}

fn find_chr(p: &Peripheral, uuid: Uuid) -> Result<btleplug::api::Characteristic> {
    p.characteristics()
        .into_iter()
        .find(|c| c.uuid == uuid)
        .ok_or_else(|| anyhow!("characteristic {} not found (wrong mode / service?)", uuid))
}

async fn run_session(p: &Peripheral, cli: &Cli) -> Result<()> {
    let session = SecureSession::establish(p, cli.password.as_deref())
        .await
        .context("handshake")?;
    println!("[ok] X25519 handshake complete, AES-256-GCM session established");

    match &cli.cmd {
        Cmd::Status => {
            // Status characteristic は 2 バイト: [flags, wifi_connected]
            // (gatt_settings::compute_status_locked と一致)。
            let bytes = session.read_chr(p, CHR_STATUS).await?;
            println!("status raw: {}", hex::encode(&bytes));
            if bytes.len() >= 2 {
                let f = bytes[0];
                println!(
                    "  ssid_set={} wifi_pw_set={} openai_key_set={} openai_on={} gemini_key_set={} provider_gemini={}",
                    f & 0x01 != 0, f & 0x02 != 0, f & 0x04 != 0,
                    f & 0x08 != 0, f & 0x10 != 0, f & 0x20 != 0,
                );
                println!("  wifi_connected={}", bytes[1] != 0);
            }
        }
        Cmd::SetMode { mode, no_apply } => {
            if *mode > 5 {
                bail!("operation mode out of range: {} (valid 0..5)", mode);
            }
            session.write_chr(p, CHR_OPERATION_MODE, &[*mode]).await?;
            println!("[ok] staged operation-mode = {}", mode);
            if *no_apply {
                println!("--no-apply: staged only, NOT rebooted (change takes effect on next Apply)");
            } else {
                session.write_chr(p, CHR_APPLY, &[0x01]).await?;
                println!("[ok] Apply written — device saves NVS and reboots in ~200ms");
            }
        }
        Cmd::Read { uuid } => {
            let bytes = session.read_chr(p, *uuid).await?;
            println!("hex : {}", hex::encode(&bytes));
            println!("utf8: {}", String::from_utf8_lossy(&bytes));
        }
        Cmd::Write { uuid, hex } => {
            let plain = hex::decode(hex.replace(' ', "")).context("value is not valid hex")?;
            session.write_chr(p, *uuid, &plain).await?;
            println!("[ok] wrote {} byte(s) to {}", plain.len(), uuid);
        }
        Cmd::Ota { file, chunk, check_every } => {
            ota(p, &session, file, *chunk, *check_every).await?;
        }
        Cmd::Fetch { tag } => {
            let ctrl = find_chr(p, CHR_OTA_CONTROL)?;
            let cmd = format!(r#"{{"op":"fetch","tag":"{}"}}"#, tag);
            p.write(&ctrl, &session.encrypt(cmd.as_bytes())?, WriteType::WithResponse)
                .await
                .context("write OtaControl fetch")?;
            let st = ota_status(p, &ctrl, &session).await?;
            println!("fetch -> {}", st);
            if st.contains("rebooting to recovery") {
                println!("(device is rebooting into Recovery, which fetches the tag over Wi-Fi)");
            }
        }
        Cmd::Scan => unreachable!(),
    }
    Ok(())
}

async fn ota_status(
    p: &Peripheral,
    ctrl: &btleplug::api::Characteristic,
    s: &SecureSession,
) -> Result<String> {
    let raw = p.read(ctrl).await.context("read OtaControl")?;
    let json = s.decrypt(&raw)?;
    Ok(String::from_utf8_lossy(&json).into_owned())
}

/// OTA: begin → OtaData に暗号チャンクを WriteWithoutResponse で流し、check_every
/// ごとに OtaControl (状態 JSON) を読んで received が追従しているか確認 → end。
/// 端末側のプロトコルは components/config_service/ota.cpp。
async fn ota(
    p: &Peripheral,
    session: &SecureSession,
    file: &std::path::Path,
    chunk: usize,
    check_every: usize,
) -> Result<()> {
    let image = std::fs::read(file).with_context(|| format!("read {}", file.display()))?;
    let ctrl = find_chr(p, CHR_OTA_CONTROL)?;
    let data = find_chr(p, CHR_OTA_DATA)?;

    let begin = format!(r#"{{"op":"begin","size":{}}}"#, image.len());
    p.write(&ctrl, &session.encrypt(begin.as_bytes())?, WriteType::WithResponse)
        .await
        .context("write OtaControl begin")?;
    let st = ota_status(p, &ctrl, session).await?;
    println!("begin -> {}", st);
    if st.contains("rebooting to recovery") {
        bail!("device is handing off to Recovery — wait ~10 s and run `ota` again");
    }
    if !st.contains(r#""state":"receiving""#) {
        bail!("device did not enter receiving state: {}", st);
    }

    let started = std::time::Instant::now();
    let mut sent = 0usize;
    for (i, c) in image.chunks(chunk).enumerate() {
        let wire = session.encrypt(c)?;
        p.write(&data, &wire, WriteType::WithoutResponse)
            .await
            .with_context(|| format!("write OtaData chunk {}", i))?;
        sent += c.len();
        if (i + 1) % check_every == 0 {
            // 端末が追いつくまで待つ (受信済みバイト数が sent と一致するまで)。
            let mut st = ota_status(p, &ctrl, session).await?;
            let mut tries = 0;
            while !st.contains(&format!(r#""received":{},"#, sent)) {
                if st.contains(r#""state":"failed""#) {
                    bail!("device reported failure: {}", st);
                }
                tries += 1;
                if tries > 50 {
                    bail!("device did not catch up (sent {} B): {}", sent, st);
                }
                tokio::time::sleep(Duration::from_millis(50)).await;
                st = ota_status(p, &ctrl, session).await?;
            }
            let secs = started.elapsed().as_secs_f64();
            print!("\r  {} / {} B ({:.0} KiB/s)   ", sent, image.len(), sent as f64 / 1024.0 / secs.max(0.001));
            use std::io::Write as _;
            let _ = std::io::stdout().flush();
        }
    }
    println!();
    // 残りのチャンクが到着するまで待ってから end。
    let mut st = ota_status(p, &ctrl, session).await?;
    let mut tries = 0;
    while !st.contains(&format!(r#""received":{},"#, sent)) {
        if st.contains(r#""state":"failed""#) {
            bail!("device reported failure: {}", st);
        }
        tries += 1;
        if tries > 100 {
            bail!("device did not receive everything: {}", st);
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
        st = ota_status(p, &ctrl, session).await?;
    }
    p.write(&ctrl, &session.encrypt(br#"{"op":"end"}"#)?, WriteType::WithResponse)
        .await
        .context("write OtaControl end")?;
    // end の後は 500 ms で再起動するので、状態読み取りは失敗しても構わない。
    match ota_status(p, &ctrl, session).await {
        Ok(st) => println!("end -> {}", st),
        Err(_) => println!("end -> (device rebooting)"),
    }
    println!(
        "[ok] {} bytes in {:.1} s",
        image.len(),
        started.elapsed().as_secs_f64()
    );
    Ok(())
}
