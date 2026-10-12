# ข้อมูลที่ยืนยันแล้ว (อัปเดต 2026-10-11)

ทุกข้อด้านล่างตรวจจากไฟล์ที่คุณส่งมาจริง (`nm`, `readelf`, `strings`, อ่านโค้ด) ถ้าข้อไหนยังไม่ได้ตรวจจะเขียนว่า "ยังไม่ยืนยัน"

## 1. GTA SA — `lib.zip` (arm64-v8a)

- `libGTASA.so` ไม่ถูก strip และมี debug info มี dynsym 21,652 ชื่อ → เรียก/ฮุกฟังก์ชันด้วย **ชื่อ**
  ผ่าน `aml->GetSym` ได้ (ProMenu ทำแบบนี้อยู่แล้ว) ไม่ต้องผูกกับ offset ของไฟล์เดียว
- offset ใน findings เดิมตรงกับ `nm` อีกครั้ง: FindPlayerPed `0x4EFAE0`, FindPlayerCoors `0x4EFEA8`,
  CPopulation::AddPed `0x5CEB40`, CWorld::Add `0x507518`
- ชื่อ mangled ที่ MCBridge 0.2 ใช้ (ทั้งหมดอยู่ใน dynsym):
  `_Z13FindPlayerPedi`, `_Z17FindPlayerVehicleib`, `_ZN11CPopulation6AddPedE8ePedTypejRK7CVectorb`,
  `_ZN10CModelInfo12GetModelInfoEPKcPi`, `_ZN10CStreaming12RequestModelEii`,
  `_ZN10CStreaming22LoadAllRequestedModelsEb`, `_ZN7CObject6CreateEib`, `_ZN6CWorld3AddEP7CEntity`,
  ฮุก `_ZN6CClock6UpdateEv`, ข้อมูล `gGameState`, `gMobileMenu`
- มีแล้วแต่ยังไม่ได้ใช้: `CCamera::TakeControl/TakeControlNoEntity/SetCamPositionForFixedMode/Restore`, `TheCamera`,
  `RwIm3DTransform`, `RwIm2DRenderPrimitive`, `RwTextureRead`, `RpClumpStreamRead`, `RpClumpRender`,
  `CEventHandler::ComputeDamageResponse`, `CPedDamageResponseCalculator` (ดูตารางเดิมใน git history)
- **ไม่พบ** `CPlaceable::SetPosn` → ตอนวางวัตถุเขียนตำแหน่งตรงตาม layout ที่ ProMenu ใช้อ่านอยู่ (ยังไม่ยืนยันว่าเขียนได้ถูก)
- `libGTASA.so` ต้องการ `libstdc++.so` และ **ไม่มี `libc++_shared.so`** ในโฟลเดอร์ lib → ปลั๊กอินต้อง build ด้วย `c++_static`
  (Application.mk เดิมใช้ `c++_shared` ซึ่งน่าจะทำให้โหลดไม่ขึ้น)
- ABI: ProMenu บันทึกไว้ในโค้ดว่า `CVector` ถูกส่งผ่านพอยน์เตอร์ใน build นี้ และ README ของ ProMenu ระบุว่าเวอร์ชัน 0.6 เด้งเพราะ
  เรียกฟังก์ชันพิกัดผิดวิธี (ไม่ได้ระบุว่าฟังก์ชันไหน) → โค้ดเดิม (`mod_main.cpp`) ที่เรียก `FindPlayerCoors` แล้วรับ struct ธรรมดา
  **ยังไม่ยืนยันว่าถูก และเสี่ยง** จึงเปลี่ยนไปอ่านตำแหน่งจากตัวเอนทิตีผ่าน `SafeMem` เหมือน ProMenu 0.7+
- `mod_main.cpp` เดิม spawn จาก pthread อื่นหลัง `sleep(8)` ไม่ใช่ game thread (เกมไม่ thread-safe) → MCBridge ทำงานใน hook `CClock::Update`
- ตำแหน่งวางปลั๊กอิน AML: `Android/data/com.rockstargames.gtasa/mods/` (จาก README ของ ProMenu)

## 2. Minecraft — `libmc.zip`

- ไฟล์ zip **เสียหาย** (ถูกตัดท้าย ไม่มี central directory) กู้ได้ครบ 12 ไฟล์ (CRC ผ่าน) ส่วน `libpreloader.so`
  ได้แค่ 123,767 จาก 15,501,040 ไบต์ → ใช้ไม่ได้
- **ไม่มี `libminecraftpe.so`** (เอนจินหลักของ Minecraft Bedrock) และไม่มีไฟล์ไหนอ้างถึงมัน
- ไลบรารีที่มีเป็นของ **LeviLauncher** (`org.levimc.launcher`): `libinbuiltmods.so` export JNI
  `...inbuilt.nativemod.{AutoSprint,Fps,Gyro,HotbarSlot,MoreButtons,PojavControls,Snaplook,Zoom}Mod_*`
  และ import `pl::memory::hook`, `pl::memory::resolveSignature`, `pl::memory::resolveVtableFunction` จาก `libpreloader.so`
  → ม็อดฝั่ง Minecraft ถูกโหลดผ่าน preloader แล้ว hook ด้วย signature scan
- ผลที่ตามมา: จะอ่าน/เขียนข้อมูลโลก Minecraft (ผู้เล่น บล็อก ม็อบ) ต้องมี `libminecraftpe.so` ของเวอร์ชันที่ใช้จริง
  และตัว SDK/header ของ preloader — **ตอนนี้ยังไม่มี จึงยังเขียนฝั่ง Minecraft ไม่ได้**

## 3. สถาปัตยกรรมที่เลือก

เกมสองตัวเป็นคนละแอป คนละโปรเซส (GTA SA = `com.rockstargames.gtasa` ผ่าน AML, Minecraft = โปรเซสของ LeviLauncher)
จึงใช้ **สะพาน loopback TCP (127.0.0.1:47800)** ให้สองปลั๊กอินคุยกันเรียลไทม์:
`GTA → MC` ตำแหน่งผู้เล่น/รถ/คนเดินถนน, `MC → GTA` สถานะเอนทิตีและบล็อก (โปรโตคอลอยู่หัวไฟล์ `mod/src/bridge_net.h`)

ทางที่ยังไม่ได้ตัดทิ้งด้วยหลักฐาน: โหลดเอนจิน Minecraft เข้าโปรเซสของ GTA โดยตรง (ไม่น่าเวิร์ก เพราะ Bedrock ต้องใช้คลาส Java
ของตัวเอง แต่ยังไม่ยืนยันเพราะไม่มี `libminecraftpe.so`)

## 4. สถานะ milestone (ตามหลักฐานจริง)

| # | งาน | สถานะ |
|---|---|---|
| 1 | ปลั๊กอิน GTA ที่ build/โหลด/เขียน log ได้ + สะพาน TCP + spawn ped ทดสอบ | เขียนแล้ว · ทดสอบตรรกะบน Linux ด้วยเกมจำลองผ่าน · **ยังไม่ build ด้วย NDK/AML จริง และยังไม่ได้รันในเกม** |
| 2 | ฝั่ง Minecraft เชื่อมสะพานและส่งตำแหน่งผู้เล่น Minecraft จริง | **รอไฟล์ + ข้อมูล (ข้อ 5)** |
| 3 | GTA วาดบล็อก Minecraft จากข้อมูลโลกจริง (RenderWare Im3D / object) | ยังไม่เริ่ม |
| 4 | Steve + ม็อบเป็น ped/clump ใน GTA (ต้องโหลดโมเดล/เท็กซ์เจอร์เอง) และโต้ตอบกับรถ/คนเดินถนน | ยังไม่เริ่ม |
| 5 | กล้อง เอฟเฟกต์ ซิงก์สองทาง | ยังไม่เริ่ม |

## 5. ข้อมูลที่ยังขาด

1. `libminecraftpe.so` + `libpreloader.so` ที่สมบูรณ์ + เวอร์ชัน Minecraft และเวอร์ชัน LeviLauncher
2. ภาพ/วิดีโอตัวอย่างที่ตั้งใจแนบ (ในแชทนี้มีแค่ไฟล์ zip 4 ไฟล์)
3. ผลรันจริง: build ผ่านไหม เกมโหลดปลั๊กอินไหม และไฟล์ `last_action.txt` / `session.log`


## ผลวิเคราะห์ `libminecraftpe.so` ของผู้ใช้ (2026-10-11) — ตรวจเองจากไฟล์

- ELF ARM64 stripped 328,419,680 ไบต์; `.text` ที่ไฟล์ออฟเซ็ต/ที่อยู่ 0x64c64e0 ขนาด 0xc44afcc (ไฟล์นี้ออฟเซ็ต = vaddr ใน `.text`)
- dynsym 90,653 ชื่อ ไม่มีคลาสเกมเลย (ClientInstance/LocalPlayer/Actor/GameMode = 0 ผล) → hook ด้วยชื่อไม่ได้ ต้องใช้ signature
- ชื่อ RTTI ของคลาสเกมอยู่ใน `.rodata` (`14ClientInstance`, `11LocalPlayer`, `11BlockSource`, `12ServerPlayer`, `5Actor`, `5Level`, `8GameMode`, `13MinecraftGame`)
- **แหล่งอ้างอิงโอเพนซอร์ส** (clone ด้วย git ได้ใน sandbox):
  - `LiteLDev/LeviLaunchroid` (Apache-2.0) + `LiteLDev/preloader-android`: API ม็อดเนทีฟ (`pl/Mod.hpp`, `pl/memory/Hook.hpp`, `pl/memory/Signature.hpp`),
    manifest `type: preload-native`, `.levipack` = zip ของ manifest.json + .so
  - `QYCottage/BedrockTools` (GPL-3.0): ม็อดเนทีฟจริงที่ฮุก Minecraft 1.26.5X.X ด้วย signature; ใช้เป็น **ข้อมูลอ้างอิง** (ชื่อฟังก์ชัน/ออฟเซ็ต) ไม่ได้คัดลอกโค้ดมาใช้
    (ถ้าจะเอาโค้ดของมันมารวม ผลงานต้องเป็น GPL-3.0 ตามไปด้วย)
- สแกน signature ทั้ง 124 ตัวของ BedrockTools กับ `.text` ของไฟล์นี้: **ตรงแบบไม่ซ้ำ 92 ตัว**, ซ้ำหลายที่ 6, ไม่ตรง 26 (VersionString และ LevelInit ไม่ตรง)
  ตัวที่เราใช้/ต้องการตรงทั้งหมด: NormalTick 0xaac7dd8, ClientInstanceUpdate 0x98036a4, ClientInstanceGetLocalPlayer 0x9808050,
  BlockSourceGetBlock 0xfb91918, LevelGetHitResult 0xfb3adb8, ActorIsPlayer 0xf56b6e0, ActorFetchNearbyActorsSorted 0xf591810
  (หน้าที่ของแต่ละตัวมาจากชื่อใน BedrockTools — เรายังไม่ได้พิสูจน์เองทีละฟังก์ชัน)
- **signature ของเราเอง** (ทุกคำสั่งเก็บแค่ไบต์บนสุด ที่เหลือเป็น wildcard, 16 คำสั่ง) ตรงไฟล์นี้ **1 ที่คือ 0xaac7dd8 เท่ากับ NormalTick ของ BedrockTools** — ยืนยันตัวตนฟังก์ชันจากสองทางอิสระ
- ออฟเซ็ตตำแหน่ง `Actor+0x208` (ตัวชี้ไปยัง state-vector component, Vec3 อยู่ที่ +0): ในไฟล์มีฟังก์ชันเล็กที่ 0xf56bbec
  `ldr x8,[x0,#0x208]; ldp s0,s1,[x8]; ldr s2,[x8,#8]; ret` ซึ่งอ่านตามโครงสร้างนี้ตรงๆ (= Actor::getPosition) และมีอีกสองฟังก์ชันเล็กที่ใช้ออฟเซ็ตเดียวกัน (0xf564370, 0xf56b054)
  ออฟเซ็ตของ rotation (0x218) **ยังไม่ยืนยัน** (มี getter 0x218 อยู่ 18 ตัวในหลายคลาส แต่ไม่เจอตัวที่คืนค่า Vec2) → ม็อดยังไม่ส่งทิศหันหน้า
- เวอร์ชัน: สตริง `1.26.52` อยู่ในไบนารี 1 ที่ และ README ของ BedrockTools ระบุเป้าหมาย 1.26.5X.X; `bedrock-samples-main.zip` ที่ผู้ใช้ส่งมาเป็นชุดตัวอย่าง 1.26.50.4 (15-09-2026)
  → น่าจะเป็นตระกูลเดียวกัน แต่ **เลขเวอร์ชันเต็มของ build นี้ยังไม่ยืนยัน** (ขอให้ผู้ใช้ดูเลขในเกม)
- `libpairipcore.so` (ระบบป้องกันแอปของ Google) เป็น dependency ของ `libminecraftpe.so` — ไม่ได้แตะ
