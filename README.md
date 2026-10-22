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

AFD reads both Unix and Windows email line endings and recovers common export
formatting errors. The reader shows recovery notes; Raw and DKIM checks always
use the original bytes. If an older version skipped emails while scanning,
open the archive, choose **Settings → Reset search data…**, then **Scan emails**.
Resetting search keeps your mail files and backs up the old search data.

For a local corpus check, build the optional `afd_mail_audit` target with
`BUILD_TESTING=ON`, then run `afd_mail_audit <email-folder> <report.json> [prefix-MiB]`.
It samples complete messages from the start of every `.mbox` file (2 MiB per file
by default). It leaves mail files untouched and reports counts, errors and
example offsets, without copying message bodies into the report.
