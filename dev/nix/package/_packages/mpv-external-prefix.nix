{
  symlinkJoin,
  lib,
  mpv-unwrapped,
}:
symlinkJoin {
  pname = "mpv-external-prefix";
  inherit (mpv-unwrapped) version;
  paths = [
    (lib.getDev mpv-unwrapped)
    (lib.getLib mpv-unwrapped)
  ];
}
