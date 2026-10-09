# ScreenShare 1.0.8

- Preserve stereo audio over WebRTC. Shared audio no longer defaults to averaging
  the left and right channels into mono, which could lower one-sided sounds and
  cancel opposite-phase content.
- Preserve front-channel levels when sharing system audio from 5.1 or 7.1
  outputs. Remove the constant downmix attenuation and reduce a mixed block only
  when it would clip, preserving stereo balance and waveform shape within it.
- Add end-to-end checks for received audio levels and channel separation through
  the shipping media engine and encrypted Opus transport at 100% viewer volume.
  Mono codec negotiation remains supported for older peers.

Regression checks cover equal, left-only, right-only and opposite-phase audio;
mono and stereo capture; 5.1/7.1 downmix; overload protection; audio source changes;
mute/volume controls; room lifecycle; and native room UI. Synthetic received
levels were within about 2% of the source. These checks do not establish equal
physical loudness between two computers; Windows mixer and device settings still
apply. Use the updated build on both ends for stereo sharing.

Setup and portable packages use the existing signed update manifest. Application
and Setup executables are not Authenticode-signed. Qt source archives are
included as release assets.
