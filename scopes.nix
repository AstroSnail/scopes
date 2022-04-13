{ lib, stdenv, makeWrapper, genie, glibc, llvmPackages_13
, llvmpkgs ? llvmPackages_13, spirv-tools, scopes-src, spirv-cross-src }:

stdenv.mkDerivation {
  name = "scopes";
  src = scopes-src;
  enableParallelBuilding = true;

  buildInputs = [
    llvmpkgs.clang
    llvmpkgs.libclang
    llvmpkgs.llvm.dev
    # llvmpkgs.llvm-polly
    spirv-tools
    genie
    makeWrapper
  ];

  configurePhase = ''
    # Only source is needed of spirv-cross
    ln --symbolic ${spirv-cross-src} SPIRV-Cross

    # Pretend that we built spirv-tools
    mkdir --parents SPIRV-Tools/build/source/opt
    ln --symbolic --target-directory=SPIRV-Tools/build/source     ${spirv-tools}/lib/libSPIRV-Tools.a
    ln --symbolic --target-directory=SPIRV-Tools/build/source/opt ${spirv-tools}/lib/libSPIRV-Tools-opt.a

    genie gmake
  '';

  makeFlags = [ "-C build" "config=release" ];

  installPhase = ''
    install -D --target-directory="$out/bin" bin/scopes
    wrapProgram $out/bin/scopes --suffix NIX_CFLAGS_COMPILE "" " -isystem ${llvmpkgs.clang}/resource-root/include/ -isystem ${
      lib.getDev stdenv.cc.libc
    }/include/"
    install -D --target-directory="$out/lib" bin/libscopesrt.so
    cp -r ./lib/scopes $out/lib/scopes
    echo ${llvmpkgs.clang} >> $out/clangpath
    cp -r ./ $out/builddump
  '';

  checkInputs = [ glibc.dev ];

  checkPhase = ''
    SCOPES_CACHE=./scopes_cache bin/scopes testing/test_all.sc
  '';
  doCheck = false;
}
