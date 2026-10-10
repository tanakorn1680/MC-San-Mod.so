# GTA SA × Minecraft Mobile — สถานะและวิธีทดสอบ

เป้าหมาย: ให้ Minecraft ทำงานคู่กับ GTA San Andreas บนมือถือ Android ซิงก์เรียลไทม์ (Steve กล้อง บล็อก ม็อบ เอฟเฟกต์
ในโลก GTA และโต้ตอบกับรถ/คนเดินถนน) — ไม่ใช่แค่เลียนแบบหน้าตา

รายละเอียดหลักฐาน/สถาปัตยกรรม/ข้อมูลที่ขาด: `docs/findings.md`

## สิ่งที่มีตอนนี้ (MCBridge 0.2 — ฝั่ง GTA เท่านั้น)

- ปลั๊กอิน AML `libMCBridge64.so` ทำงานบน game thread (ฮุก `CClock::Update`) และหาฟังก์ชันเกมด้วยชื่อ
- เปิดเซิร์ฟเวอร์ `127.0.0.1:47800` ส่งตำแหน่งผู้เล่น ~10 ครั้ง/วินาที และรับคำสั่งทดสอบ
- เขียน log ทุกขั้นลง `Android/data/com.rockstargames.gtasa/files/MCBridge/`
  - `last_action.txt` 14 ขั้นล่าสุด (เขียนทับทุกครั้ง รอดแม้เกมเด้ง) · `previous_session.txt` ของรอบก่อน
  - `session.log` ล็อกเต็มรอบนี้ · `previous_session.log` ของรอบก่อน
  - `config.txt` สร้างให้ตอนรันครั้งแรก แก้ได้บนมือถือ (พอร์ต, เปิด/ปิด self test, ชื่อโมเดลที่จะลองค้น)

**ยังไม่ได้ build ด้วย NDK จริง และยังไม่เคยรันในเกม** (ตรวจได้เฉพาะตรรกะบนเครื่อง Linux ด้วยเกมจำลอง)

## ขั้นตอนทดสอบ

1. อัปโหลดไฟล์ในชุดนี้ขึ้น repo (branch `main`) → Actions จะ build เอง (หรือกด Run workflow)
2. ดาวน์โหลด artifact `libMCBridge64` วางใน `Android/data/com.rockstargames.gtasa/mods/`
   ถ้า build พัง ส่งไฟล์ artifact `build-log` (หรือข้อความใน "First build errors") กลับมา
3. เข้าเกม เดินในที่โล่ง รอประมาณ 10–20 วินาที (self test: อ่านตำแหน่ง, ค้นชื่อโมเดล, spawn คนเดินถนน 1 คนข้างๆ)
4. ส่งกลับมา: `last_action.txt` + `session.log` (และ `previous_session.txt` ถ้าเกมเด้ง)
5. ทดสอบสะพาน (Termux): `python tools/bridge_probe.py --send PING --send SPAWN_PED --seconds 5`

คำสั่งที่รับ: `PING`, `SPAWN_PED`, `SPAWN_OBJ <ชื่อโมเดล>` (วัตถุเสี่ยงกว่า ปิดไว้ใน self test; เปิดด้วย `selftest_obj=1`)
