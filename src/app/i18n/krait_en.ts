<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="en">
<context>
    <name>Banner</name>
    <message>
        <location filename="../qml/Banner.qml" line="20"/>
        <source>Allow (Ctrl+Enter)</source>
        <translation>Allow (Ctrl+Enter)</translation>
    </message>
    <message>
        <location filename="../qml/Banner.qml" line="21"/>
        <source>Cancel</source>
        <translation>Cancel</translation>
    </message>
</context>
<context>
    <name>ErrorBanner</name>
    <message>
        <location filename="../error_banner.h" line="41"/>
        <source>Could not create the pseudoconsole.</source>
        <translation>Could not create the pseudoconsole.</translation>
    </message>
    <message>
        <location filename="../error_banner.h" line="42"/>
        <source>The bundled OpenConsole may be missing from the openconsole folder beside Krait.</source>
        <translation>The bundled OpenConsole may be missing from the openconsole folder beside Krait.</translation>
    </message>
    <message>
        <location filename="../error_banner.h" line="50"/>
        <source>Could not start the shell.</source>
        <translation>Could not start the shell.</translation>
    </message>
    <message>
        <location filename="../error_banner.h" line="51"/>
        <source>Check that the configured shell exists and is executable.</source>
        <translation>Check that the configured shell exists and is executable.</translation>
    </message>
    <message>
        <location filename="../error_banner.h" line="58"/>
        <source>Lost contact with the shell.</source>
        <translation>Lost contact with the shell.</translation>
    </message>
    <message>
        <location filename="../error_banner.h" line="66"/>
        <source>The session ended unexpectedly.</source>
        <translation>The session ended unexpectedly.</translation>
    </message>
    <message>
        <location filename="../error_banner.h" line="67"/>
        <source>The console host closed while the shell was still running.</source>
        <translation>The console host closed while the shell was still running.</translation>
    </message>
    <message>
        <location filename="../error_banner.h" line="74"/>
        <source>Could not reach the server.</source>
        <translation>Could not reach the server.</translation>
    </message>
    <message>
        <location filename="../error_banner.h" line="75"/>
        <source>Check the host name and port, and that the network is up.</source>
        <translation>Check the host name and port, and that the network is up.</translation>
    </message>
    <message>
        <location filename="../error_banner.h" line="87"/>
        <source>This server is presenting a different identity than the one Krait remembers.</source>
        <translation>This server is presenting a different identity than the one Krait remembers.</translation>
    </message>
    <message>
        <location filename="../error_banner.h" line="90"/>
        <source>That happens when a server is rebuilt or its key is replaced — and it is also what an interception looks like. Ask whoever runs the server before you connect again, and do not type a password until you have.</source>
        <translation>That happens when a server is rebuilt or its key is replaced — and it is also what an interception looks like. Ask whoever runs the server before you connect again, and do not type a password until you have.</translation>
    </message>
    <message>
        <location filename="../error_banner.h" line="100"/>
        <source>The server&apos;s identity was not accepted, so nothing was sent to it.</source>
        <translation>The server's identity was not accepted, so nothing was sent to it.</translation>
    </message>
    <message>
        <location filename="../error_banner.h" line="103"/>
        <source>Compare the fingerprint with one you got from a source other than this connection, then try again.</source>
        <translation>Compare the fingerprint with one you got from a source other than this connection, then try again.</translation>
    </message>
    <message>
        <location filename="../error_banner.h" line="112"/>
        <source>The server did not accept these credentials.</source>
        <translation>The server did not accept these credentials.</translation>
    </message>
    <message>
        <location filename="../error_banner.h" line="114"/>
        <source>Check the user name, and whether this profile should be using a key or the agent instead of a password.</source>
        <translation>Check the user name, and whether this profile should be using a key or the agent instead of a password.</translation>
    </message>
    <message>
        <location filename="../error_banner.h" line="125"/>
        <source>The session failed.</source>
        <translation>The session failed.</translation>
    </message>
</context>
<context>
    <name>Main</name>
    <message>
        <location filename="../qml/Main.qml" line="9"/>
        <source>Krait</source>
        <translation>Krait</translation>
    </message>
    <message>
        <location filename="../qml/Main.qml" line="51"/>
        <source>Dismiss</source>
        <translation>Dismiss</translation>
    </message>
    <message>
        <location filename="../qml/Main.qml" line="60"/>
        <source>Paste anyway</source>
        <translation>Paste anyway</translation>
    </message>
    <message>
        <location filename="../qml/Main.qml" line="61"/>
        <source>Cancel</source>
        <translation>Cancel</translation>
    </message>
</context>
<context>
    <name>PasteGuard</name>
    <message>
        <location filename="../input/paste.cpp" line="121"/>
        <source>This paste contains a command that can destroy data or escalate privileges. Read it before allowing it.</source>
        <translation>This paste contains a command that can destroy data or escalate privileges. Read it before allowing it.</translation>
    </message>
    <message>
        <location filename="../input/paste.cpp" line="125"/>
        <source>This paste ends with a newline, so the shell will run it immediately.</source>
        <translation>This paste ends with a newline, so the shell will run it immediately.</translation>
    </message>
    <message>
        <location filename="../input/paste.cpp" line="128"/>
        <source>This paste has more than one line. Every line will run.</source>
        <translation>This paste has more than one line. Every line will run.</translation>
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
        <translation>portable (beside the executable)</translation>
    </message>
    <message>
        <location filename="../settings/paths.cpp" line="60"/>
        <source>user profile</source>
        <translation>user profile</translation>
    </message>
</context>
<context>
    <name>krait::app::TerminalItem</name>
    <message>
        <location filename="../terminal_item.cpp" line="99"/>
        <source>The shell exited with code %1.</source>
        <translation>The shell exited with code %1.</translation>
    </message>
</context>
<context>
    <name>krait::net::ConptyBackend</name>
    <message>
        <location filename="../../net/conpty/conpty_backend.cpp" line="197"/>
        <source>The console host closed unexpectedly. The session is over; the shell may still be running.</source>
        <translation>The console host closed unexpectedly. The session is over; the shell may still be running.</translation>
    </message>
</context>
<context>
    <name>krait::net::SshBackend</name>
    <message>
        <location filename="../../net/ssh/ssh_backend.cpp" line="80"/>
        <source>This session has no host to connect to.</source>
        <translation>This session has no host to connect to.</translation>
    </message>
    <message>
        <location filename="../../net/ssh/ssh_backend.cpp" line="107"/>
        <source>Gave up reconnecting to %1 after %2 attempts.</source>
        <translation>Gave up reconnecting to %1 after %2 attempts.</translation>
    </message>
    <message>
        <location filename="../../net/ssh/ssh_backend.cpp" line="166"/>
        <source>Could not create an SSH session.</source>
        <translation>Could not create an SSH session.</translation>
    </message>
    <message>
        <location filename="../../net/ssh/ssh_backend.cpp" line="218"/>
        <source>The server did not present a host key.</source>
        <translation>The server did not present a host key.</translation>
    </message>
    <message>
        <location filename="../../net/ssh/ssh_backend.cpp" line="234"/>
        <source>The server&apos;s host key could not be read.</source>
        <translation>The server's host key could not be read.</translation>
    </message>
    <message>
        <location filename="../../net/ssh/ssh_backend.cpp" line="283"/>
        <source>The host key for %1 has CHANGED. The connection was stopped.</source>
        <translation>The host key for %1 has CHANGED. The connection was stopped.</translation>
    </message>
    <message>
        <location filename="../../net/ssh/ssh_backend.cpp" line="285"/>
        <source>%1 offered a host key of a different type than the one on record.</source>
        <translation>%1 offered a host key of a different type than the one on record.</translation>
    </message>
    <message>
        <location filename="../../net/ssh/ssh_backend.cpp" line="293"/>
        <source>No answer about the host key; connection stopped.</source>
        <translation>No answer about the host key; connection stopped.</translation>
    </message>
    <message>
        <location filename="../../net/ssh/ssh_backend.cpp" line="302"/>
        <source>The host key was not accepted.</source>
        <translation>The host key was not accepted.</translation>
    </message>
    <message>
        <location filename="../../net/ssh/ssh_backend.cpp" line="306"/>
        <source>The host key could not be saved to known_hosts.</source>
        <translation>The host key could not be saved to known_hosts.</translation>
    </message>
    <message>
        <location filename="../../net/ssh/ssh_backend.cpp" line="357"/>
        <source>Passphrase for %1</source>
        <translation>Passphrase for %1</translation>
    </message>
    <message>
        <location filename="../../net/ssh/ssh_backend.cpp" line="450"/>
        <source>Password for %1@%2</source>
        <translation>Password for %1@%2</translation>
    </message>
    <message>
        <location filename="../../net/ssh/ssh_backend.cpp" line="541"/>
        <source>%1 refused every authentication method we could offer.</source>
        <translation>%1 refused every authentication method we could offer.</translation>
    </message>
    <message>
        <location filename="../../net/ssh/ssh_backend.cpp" line="550"/>
        <source>Could not open a channel.</source>
        <translation>Could not open a channel.</translation>
    </message>
    <message>
        <location filename="../../net/ssh/ssh_backend.cpp" line="655"/>
        <source>%1 stopped responding.</source>
        <translation>%1 stopped responding.</translation>
    </message>
</context>
</TS>
