# Security

VitaSend receives files from other devices on your network and can install `.vpk` packages. Please read these notes
before using it on a network you do not control.

## Things to know

* **Quick Save** and **automatic installation** trust the name and fingerprint a sender *announces*; with self-signed
  certificates this cannot be verified. Keep both on *Ask* on any network you do not trust.
* **A `.vpk` can run anything** on your Vita. The installer shows what a package is and whether it asks for extended
  permissions, but it cannot judge whether it is safe. Only install packages from sources you trust.
* The app itself runs with **extended permissions** ("unsafe homebrew") because the system installer needs them.
* With a PIN set, senders must enter it; five wrong attempts block everyone for a minute.
* Received file names are sanitised, and the `.vpk` unpacker refuses names that would leave the scratch folder.

## Reporting a vulnerability

Please do **not** open a public issue for a security problem. Use GitHub's *"Report a vulnerability"* button on the
repository's **Security** tab (private vulnerability reporting). Describe what you found, how to reproduce it and which
version you tested. You will get an answer as soon as possible; this is a spare-time project, so please be patient.

Only the latest release is supported.
