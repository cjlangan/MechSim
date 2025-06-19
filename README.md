<h1 align="center">mechsim</h1>

<div align="center">
<p>
<a href="https://github.com/cjlangan/mechsim/stargazers"><img src="https://img.shields.io/github/stars/cjlangan/mechsim?style=for-the-badge&logo=starship&color=C9CBFF&logoColor=C9CBFF&labelColor=302D41" alt="stars"><a>&nbsp;&nbsp;
<a href="https://github.com/cjlangan/mechsim/"><img src="https://img.shields.io/github/repo-size/cjlangan/mechsim?style=for-the-badge&logo=hyprland&logoColor=f9e2af&label=Size&labelColor=302D41&color=f9e2af" alt="REPO SIZE"></a>&nbsp;&nbsp;
<a href="https://github.com/cjlangan/mechsim/releases"><img src="https://img.shields.io/github/v/release/cjlangan/mechsim?style=for-the-badge&logo=github&logoColor=eba0ac&label=Release&labelColor=302D41&color=eba0ac" alt="Releases"></a>&nbsp;&nbsp;
<a href="https://github.com/cjlangan/mechsim/blob/main/LICENSE"><img src="https://img.shields.io/github/license/cjlangan/mechsim?style=for-the-badge&logo=&color=CBA6F7&logoColor=CBA6F7&labelColor=302D41" alt="LICENSE"></a>&nbsp;&nbsp;
</p>
</div>

<p align="center">A CLI-Based Mechanical Keyboard Sound Simulator</p>

<br>

https://github.com/user-attachments/assets/622ff7b3-11c9-4f51-be4d-5f19565adbf5

<div align="center">
<p align="center">The video shows just a few of the many options</p>
</div>

## Usage

```bash
mechsim         # default uses sound eg-oreo at volume 20

mechsim -l      # list out all sound packs

mechsim -s turquoise -V 60  # choose soundpack turquoise at 60% volume
```

## Installation

### Arch Linux (AUR)

```bash
yay -S mechsim
```

### Build From Source

```bash
git clone https://github.com/cjlangan/mechsim
cd mechsim
make
sudo make install
```

## Full Usage

    Usage: mechsim [OPTIONS]

    Options:
      -s, --sound SOUND_NAME   Select sound pack (default: eg-oreo)
      -V, --volume VOLUME      Set volume [0-100] (default: 20)
      -l, --list               List available sound packs
      -h, --help               Show this help message
      -v, --verbose            Enable verbose output

    Examples:
      mechsim                       # Use default sound (eg-oreo)
      mechsim -s cherrymx-blue-abs  # Use Cherry MX Blue ABS sound
      mechsim -l                    # List all available sounds


## Available Sounds:

- nk-cream
- cherrymx-black-abs
- cherrymx-red-pbt
- cherrymx-blue-abs
- eg-oreo
- cherrymx-brown-pbt
- eg-crystal-purple
- cherrymx-brown-abs
- topre-purple-hybrid-pbt
- cream-travel
- holy-pandas
- mxbrown-travel
- cherrymx-red-abs
- mxblack-travel
- cherrymx-black-pbt
- turquoise
- cherrymx-blue-pbt
- mxblue-travel


## Dependencies

- build-essential
- pkg-config
- libjson-c-dev
- libpulse-dev
- libsndfile1-dev
- libinput-dev
- libevdev-dev
- libudev-d


## Motivation 

The motivation behind this project was that I couldn't hear my keybaord presses
in screen-recordings or when I had headphones on. I tried other programs such
as [MechVibes](https://github.com/hainguyents13/mechvibes) (which I took the
sound files and configs from) -- but none were Wayland compatible.

I decided to take some backend code from [Show Me The
Key](https://github.com/AlynxZhou/showmethekey) which could detect global key
presses -- even on Wayland -- and fit it to my use case.

Admittedly, I used a lot of AI to help me complete this, since I was more
interested in just getting it to work to enjoy the sweet sweet keypresses. But
I have since decided to share the code if others found themselves in a similar
position.
