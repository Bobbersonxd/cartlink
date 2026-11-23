<!-- Copyright (C) 2026 Cartlink contributors. GNU GPL v3 or later (see COPYING). -->

# Your own game server

This turns a Linux PC at home into a small game server for your friends.
They paste one link into Cartlink, log in once, and see a menu of your games.

- Every friend gets their own name and password.
- Everything goes over https (locked), and nothing can be seen without a login.
- Passwords are stored only as scrambled hashes, never as plain text.
- Your other programs and websites are not touched.

**Works on Arch Linux and Debian/Ubuntu only (for now).** It uses the Caddy
web server from your system's packages.

**Only put files on your server that you have the right to share (your own homebrew, public-domain or freely licensed games). Sharing commercial games or BIOS files you don't own the rights to is illegal in most countries.**

All commands below are run in a terminal **in the Cartlink source folder**
(the folder that has `README.md` and `tools/` in it). In the examples the
server is called `example.duckdns.org`; use your own name from step 2.

You do steps 0 to 4 once. They take about 20 minutes.

## 0. Install Caddy

Cartlink needs **Caddy 2.8 or newer**.

- Arch Linux: `sudo pacman -S caddy`
- Debian / Ubuntu: the `caddy` that comes with Debian and Ubuntu is too old
  (2.6), so install it from Caddy's own apt repository (these are the steps
  from <https://caddyserver.com/docs/install>):

      sudo apt install --yes debian-keyring debian-archive-keyring apt-transport-https curl
      curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' | sudo gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
      curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' | sudo tee /etc/apt/sources.list.d/caddy-stable.list
      sudo chmod o+r /usr/share/keyrings/caddy-stable-archive-keyring.gpg
      sudo chmod o+r /etc/apt/sources.list.d/caddy-stable.list
      sudo apt update
      sudo apt install caddy

Check it with `caddy version`: it must say `v2.8` or higher. The setup in
step 4 stops (and changes nothing) if Caddy is older.

## 1. Check that your internet can host a server

Some internet providers share one address between many homes ("CGNAT").
Then friends can't reach your PC this way, so check first.

1. Open your router's page in a browser. It is usually <http://192.168.0.1> or
   <http://192.168.1.1> (it is often printed on a sticker on the router).
2. Find the "WAN", "Internet" or "Public" IP address on its status page.
3. In a terminal, run:

       curl https://api.ipify.org

4. Compare the two numbers.
   - **Same:** good, go on to step 2.
   - **Different** (or the router's one starts with `10.`, `192.168.`,
     `100.64.` to `100.127.`, or `172.16.` to `172.31.`): **stop here.** Port forwarding will not work from your home.
     Ask your internet provider for a public IP address, or open an issue on
     Cartlink's issue tracker to ask about other ways.

## 2. Make your free web name (DuckDNS)

Your home address can change. DuckDNS gives you a fixed name that this PC keeps
pointed at your home.

1. Go to <https://www.duckdns.org> and sign in.
2. Type a name in the "sub domain" box and press **add domain**. If you typed
   `example`, your server is `example.duckdns.org`.
3. At the top of the page there is a **token** (a long code). Save it in a file
   only you can read:

       install -m 600 /dev/null ~/duckdns.token
       nano ~/duckdns.token

   Paste the token, then press Ctrl+O, Enter, Ctrl+X.

Keep the token secret. Never paste it into chats or put it in git.

## 3. Open the doors on your router (port forwarding)

Friends' requests reach your router first. Tell the router to send them on to
this PC.

1. Find this PC's home-network address:

       ip -4 route get 1.1.1.1

   The number after `src` is it (for example `192.168.1.50`).
2. On the router's page, find **Port forwarding** (sometimes under "NAT",
   "Virtual server" or "Advanced"). Add two rules, both **TCP**:
   - port **80** to `192.168.1.50` port **80**
   - port **443** to `192.168.1.50` port **443**

   (Use your own number from step 1.)
3. Also look for **DHCP reservation** or "fixed IP" and reserve that number for
   this PC, so it doesn't change and break the rules.

Port 443 is the locked https door. Port 80 is only used to get the security
certificate and to send people on to https.

## 4. Run the setup

Use your own name from step 2:

    sudo tools/cartlink-server/setup.sh example.duckdns.org ~/duckdns.token

It makes the games folder, sets up Caddy (the web server), and starts a small
job that keeps the DuckDNS name up to date every 5 minutes. It is safe to run
again. When it says **Done**, you can delete your copy of the token:

    rm ~/duckdns.token

The first time someone opens the link, Caddy gets a free security certificate
for your name. That can take a minute.

## 5. Add your friends

For each friend:

    tools/cartlink-server/cartlink-users add sam

It makes a random password of five words (like `maker-sheet-perch-stash-baked`)
and shows it **once**. Send it to that friend privately; only a scrambled hash
of it is saved, so it can't be shown again. (Lost it? Remove the friend and
add them again.)

To type a password yourself instead (at least 12 characters):

    tools/cartlink-server/cartlink-users add --choose sam

See who has a login:

    tools/cartlink-server/cartlink-users list

## 6. Put games in

Make one folder per system inside `/srv/cartlink/games/` and **copy** the games
in, for example:

    mkdir -p "/srv/cartlink/games/Game Boy"
    cp ~/MyHomebrew/*.gb "/srv/cartlink/games/Game Boy/"

Remember: only files you have the right to share (see the top of this page).

**Don't put links (symlinks) in the games folder.** Caddy follows them, so a
link to another folder would show that folder to every friend. The setup and
`cartlink-users` warn if they find one.

Friends see the folders and files with these names. Files and folders that
start with a dot are hidden.

If a game won't download, run the setup from step 4 again: it fixes the file
permissions.

## 7. What to send a friend

Send them (best in a private message):

- the link, with **https://** at the start: `https://example.duckdns.org/`
- their name and their password

Cartlink only sends logins over https with a valid certificate, so the link
must start with `https://` (not `http://`).

In Cartlink they choose **Load from URL** and paste the link. It asks for the
name and password once, then shows the game menu. After that the server is
under **Saved Servers**.

You can use the very same link on this PC too. That keeps netplay simple:
everyone loads the same file from the same link.

### Links with a fingerprint (optional)

A link can carry the file's fingerprint, so Cartlink refuses the download if
the file is not exactly the one you meant:

    tools/cartlink-server/cartlink-link "https://example.duckdns.org/Game%20Boy/My%20Game.gb" "/srv/cartlink/games/Game Boy/My Game.gb"

prints `https://example.duckdns.org/Game%20Boy/My%20Game.gb#sha256=...`.
For a link to every game at once:

    tools/cartlink-server/cartlink-link --dir /srv/cartlink/games https://example.duckdns.org/

Make new links after you change a file: the old ones no longer match it.

## 8. Remove a friend

    tools/cartlink-server/cartlink-users remove sam

They can't log in any more from that moment.

## 9. Slow down password guessing (optional, fail2ban)

The passwords from step 5 are far too long to guess, but someone can still
keep trying and make your PC busy. fail2ban blocks an address for one hour
after 10 wrong logins in 10 minutes:

1. Install it: `sudo pacman -S fail2ban` (Arch) or `sudo apt install fail2ban`
   (Debian/Ubuntu).
2. Copy Cartlink's filter and jail:

       sudo install -m 644 tools/cartlink-server/fail2ban/cartlink-filter.conf /etc/fail2ban/filter.d/cartlink.conf
       sudo install -m 644 tools/cartlink-server/fail2ban/cartlink-jail.local /etc/fail2ban/jail.d/cartlink.local

3. Start it: `sudo systemctl enable --now fail2ban` (or
   `sudo systemctl restart fail2ban` if it was already running).
4. Check that the jail is loaded: `sudo fail2ban-client status cartlink`.
   If that fails, see `journalctl -u fail2ban`.
5. Check that a ban really works, with a test address that is never used on
   the internet:

       sudo fail2ban-client set cartlink banip 192.0.2.1
       sudo fail2ban-client status cartlink
       sudo fail2ban-client set cartlink unbanip 192.0.2.1

   The middle command should list `192.0.2.1` under "Banned IP list".

A friend who opens the server for the first time gets one "wrong login"
before Cartlink asks for the password; that is far below the limit.

## 10. Turn it all off

    sudo systemctl disable --now caddy cartlink-duckdns.timer

The server stops and won't start again after a restart. Your games stay in
`/srv/cartlink/games/`. You can also delete the two port forwarding rules on
the router. To turn it back on, run the setup from step 4 again.

## Good to know

- Who downloaded what is in `/var/log/caddy/cartlink.log` (kept small
  automatically).
- Check the DuckDNS job: `journalctl -u cartlink-duckdns`
- Check Caddy: `systemctl status caddy` and `journalctl -u caddy`
- `tools/cartlink-server/setup.sh --check-system` shows what the setup found
  (Arch or Debian/Ubuntu, and whether `/etc/caddy/Caddyfile` is still the
  package's sample). It changes nothing.
- The setup only changes what is listed at the top of
  `tools/cartlink-server/setup.sh`. If Caddy still had its sample settings (a
  welcome page open to anyone), they are put aside as
  `/etc/caddy/Caddyfile.package-sample`.
- For developers: `tools/cartlink-server/test_server.sh` tests the kit as a
  normal user (no sudo) with a throwaway Caddy on `127.0.0.1:18443`.

The random-password word list is the EFF short word list; see
`WORDLIST-LICENSE.txt`.
