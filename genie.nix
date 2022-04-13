{ stdenv, genie-src }:

stdenv.mkDerivation {
  name = "genie";
  src = genie-src;
  installPhase = ''
    install -D --target-directory="$out/bin" bin/linux/genie
  '';
}
