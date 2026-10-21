# AlternativeFastDelivery

**AFD** is a native, local archive and email inspection workstation built with
C++20 and Qt Quick. Open local MBOX/EML files, index searchable plain text,
inspect raw byte ranges and verify DKIM signatures. Browse torrent metadata and
request selected verified pieces through libtorrent.

Open a file with **Open file** or **Ctrl+O**. MBOX archives open in Emails with a
bounded sample; choose **Scan emails** to make the archive searchable, then
use **Find** or **Ctrl+F**. EML files open directly in the message reader. Other
files open in **Preview**. Use **Torrent files** to return to the same folder and
position after opening a torrent file. **Close torrent** keeps downloaded files;
**Reopen torrent** continues the saved session.

On first launch, AFD asks for a download folder. Choose a folder or select Later
to work with local files first. A torrent cannot start until a folder is chosen.
Change the default for new torrents in **Settings → Download folder**. Existing
downloads stay in their original folder; Settings shows the current torrent's
actual location.