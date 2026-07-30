<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="th">
<context>
    <name>Banner</name>
    <message>
        <location filename="../qml/Banner.qml" line="20"/>
        <source>Allow</source>
        <translation>อนุญาต</translation>
    </message>
    <message>
        <location filename="../qml/Banner.qml" line="21"/>
        <source>Cancel</source>
        <translation>ยกเลิก</translation>
    </message>
</context>
<context>
    <name>ErrorBanner</name>
    <message>
        <location filename="../error_banner.h" line="41"/>
        <source>Could not create the pseudoconsole.</source>
        <translation>ไม่สามารถสร้างซูโดคอนโซลได้</translation>
    </message>
    <message>
        <location filename="../error_banner.h" line="42"/>
        <source>The bundled OpenConsole may be missing from the openconsole folder beside Krait.</source>
        <translation>OpenConsole ที่มากับโปรแกรมอาจหายไปจากโฟลเดอร์ openconsole ข้าง Krait</translation>
    </message>
    <message>
        <location filename="../error_banner.h" line="50"/>
        <source>Could not start the shell.</source>
        <translation>ไม่สามารถเริ่มเชลล์ได้</translation>
    </message>
    <message>
        <location filename="../error_banner.h" line="51"/>
        <source>Check that the configured shell exists and is executable.</source>
        <translation>ตรวจสอบว่าเชลล์ที่ตั้งค่าไว้มีอยู่จริงและรันได้</translation>
    </message>
    <message>
        <location filename="../error_banner.h" line="58"/>
        <source>Lost contact with the shell.</source>
        <translation>ขาดการติดต่อกับเชลล์</translation>
    </message>
    <message>
        <location filename="../error_banner.h" line="66"/>
        <source>The session ended unexpectedly.</source>
        <translation>เซสชันสิ้นสุดโดยไม่คาดคิด</translation>
    </message>
    <message>
        <location filename="../error_banner.h" line="67"/>
        <source>The console host closed while the shell was still running.</source>
        <translation>โฮสต์คอนโซลปิดตัวลงขณะที่เชลล์ยังทำงานอยู่</translation>
    </message>
    <message>
        <location filename="../error_banner.h" line="77"/>
        <source>The session failed.</source>
        <translation>เซสชันล้มเหลว</translation>
    </message>
</context>
<context>
    <name>Main</name>
    <message>
        <location filename="../qml/Main.qml" line="51"/>
        <source>Dismiss</source>
        <translation>ปิด</translation>
    </message>
    <message>
        <location filename="../qml/Main.qml" line="60"/>
        <source>Paste anyway</source>
        <translation>วางต่อไป</translation>
    </message>
    <message>
        <location filename="../qml/Main.qml" line="61"/>
        <source>Cancel</source>
        <translation>ยกเลิก</translation>
    </message>
</context>
<context>
    <name>PasteGuard</name>
    <message>
        <location filename="../input/paste.cpp" line="102"/>
        <source>This paste contains a command that can destroy data or escalate privileges. Read it before allowing it.</source>
        <translation>ข้อความที่วางมีคำสั่งที่อาจลบข้อมูลหรือยกระดับสิทธิ์ กรุณาอ่านให้ครบก่อนอนุญาต</translation>
    </message>
    <message>
        <location filename="../input/paste.cpp" line="106"/>
        <source>This paste ends with a newline, so the shell will run it immediately.</source>
        <translation>ข้อความที่วางลงท้ายด้วยการขึ้นบรรทัดใหม่ เชลล์จะรันคำสั่งนี้ทันที</translation>
    </message>
    <message>
        <location filename="../input/paste.cpp" line="109"/>
        <source>This paste has more than one line. Every line will run.</source>
        <translation>ข้อความที่วางมีมากกว่าหนึ่งบรรทัด ทุกบรรทัดจะถูกรัน</translation>
    </message>
</context>
<context>
    <name>Settings</name>
    <message>
        <location filename="../settings/paths.cpp" line="54"/>
        <source>KRAIT_CONFIG_DIR</source>
        <translation>KRAIT_CONFIG_DIR</translation>
    </message>
    <message>
        <location filename="../settings/paths.cpp" line="56"/>
        <source>portable (beside the executable)</source>
        <translation>แบบพกพา (อยู่ข้างไฟล์โปรแกรม)</translation>
    </message>
    <message>
        <location filename="../settings/paths.cpp" line="60"/>
        <source>user profile</source>
        <translation>โปรไฟล์ผู้ใช้</translation>
    </message>
</context>
<context>
    <name>krait::app::TerminalItem</name>
    <message>
        <location filename="../terminal_item.cpp" line="98"/>
        <source>The shell exited with code %1.</source>
        <translation>เชลล์จบการทำงานด้วยรหัส %1</translation>
    </message>
</context>
<context>
    <name>krait::net::ConptyBackend</name>
    <message>
        <location filename="../../net/conpty/conpty_backend.cpp" line="197"/>
        <source>The console host closed unexpectedly. The session is over; the shell may still be running.</source>
        <translation>โฮสต์คอนโซลปิดตัวลงโดยไม่คาดคิด เซสชันสิ้นสุดแล้ว แต่เชลล์อาจยังทำงานอยู่</translation>
    </message>
</context>
</TS>
