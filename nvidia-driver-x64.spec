# Created by Perry for Nvidia Driver .run --> .rpm 2016.11.11
%define _kmodver %(echo `uname -r`)
%define _kernver %(echo `uname -r |sed 's/-.*//'`)
#%define _kver    %(echo `uname -r |awk -F '.' '{print $3}'`)

#NVIDIA DKMS_Version
%global debug_package %{nil}
%global dkms_name nvidia
%global module_name nvidia

Summary:   Xorg X11 nvidia video driver
Name:      nvidia
Version:   367.44
Release:   rhel_7.3.el7
URL:       http://www.nvidia.com
License:   NVIDIA, distributable
Group:     User Interface/X Hardware Support
Packager:  perry_yuan<perry_yuan@dell.com>
BuildRoot: %{_tmppath}/%{name}-%{version}-root-%(%{__id_u} -n)
BuildRequires:  chkconfig coreutils grubby

Requires: prelink, coreutils, dkms
Requires: kernel = %{_kernver}
Requires: kernel-devel = %{_kernver}
BuildRequires: kernel = %{_kernver}
#BuildRequires: kernel-PAE = %{_kernver}
#BuildRequires: kernel-devel = %{_kernver}
BuildRequires: kernel-devel = %{_kernver}
#BuildRequires: kernel-headers = %{_kernver}

#Conflicts: libvdpau libvdpau-devel
Source0:   NVIDIA-Linux-x86-%{version}.tar.gz
Source1:   blacklist-nouveau.conf
Source2:   xorg.conf
Source3:   prime.desktop
ExcludeArch: s390 s390x

%description 
video Cards driver for nvidia Geforce series.

%prep
if [ -f /usr/lib64/xorg/modules/libglamoregl.so ]
then 
    if [ ! -f /usr/lib64/xorg/modules/libglamoregl.so.bak ]
    then
        cp /usr/lib64/xorg/modules/libglamoregl.so /usr/lib64/xorg/modules/libglamoregl.so.bak
    fi
fi

if [ -f /usr/lib64/xorg/modules/extensions/libglx.so ]
then 
    if [ ! -f /usr/lib64/xorg/modules/extensions/libglx.so.bak ]
    then
        cp /usr/lib64/xorg/modules/extensions/libglx.so /usr/lib64/xorg/modules/extensions/libglx.so.bak
    fi
fi


#sh %{SOURCE0} -x
tar xzf %{SOURCE0}
pushd %{_builddir}
mv NVIDIA-Linux-x86-%{version} %{name}-%{version}
popd

%build
export IGNORE_CC_MISMATCH=1

#make KERNEL_MODLIB=/lib/modules/%{_kmodver}
#make KERNEL_MODLIB=/lib/modules/%{_kmodver} module
cp -fr  %{name}-%{version}/kernel/   kernel/


%install
rm -rf $RPM_BUILD_ROOT
# Create empty tree
mkdir -p %{buildroot}%{_usrsrc}/%{dkms_name}-%{version}/
cp -fr kernel/* %{buildroot}%{_usrsrc}/%{dkms_name}-%{version}/

cd %{name}-%{version}/

#mkdir -p %{buildroot}/lib/modules/%{_kmodver}/kernel/drivers/video/
#cp kernel/nvidia.ko %{buildroot}/lib/modules/%{_kmodver}/kernel/drivers/video/nvidia.ko

mkdir -p $RPM_BUILD_ROOT/usr
mkdir -p $RPM_BUILD_ROOT/etc/X11/
mkdir -p $RPM_BUILD_ROOT/usr/bin/
mkdir -p $RPM_BUILD_ROOT/usr/lib64/mytls/
mkdir -p $RPM_BUILD_ROOT/usr/lib64/vdpau/
mkdir -p $RPM_BUILD_ROOT/usr/lib64/nvidia/xorg/modules/
mkdir -p $RPM_BUILD_ROOT/usr/lib64/nvidia/xorg/modules/drivers/
mkdir -p $RPM_BUILD_ROOT/usr/lib64/nvidia/xorg/modules/extensions/
mkdir -p $RPM_BUILD_ROOT/usr/lib64/GL/
mkdir -p $RPM_BUILD_ROOT/usr/share/nvidia/
mkdir -p $RPM_BUILD_ROOT/usr/share/applications/
#file /usr/share/man/man1 from install of nvidia-367.44-rhel7.3.x86_64 conflicts with file from package filesystem-3.2-21.el7.x86_64
#mkdir -p $RPM_BUILD_ROOT/usr/share/man/man1/
mkdir -p $RPM_BUILD_ROOT/usr/share/doc/NVIDIA_GLX-1.0/
mkdir -p $RPM_BUILD_ROOT/etc/OpenCL/vendors/
mkdir -p $RPM_BUILD_ROOT/usr/share/gdm/greeter/autostart/
#add for rhel7.3
mkdir -p $RPM_BUILD_ROOT/etc/vulkan/icd.d/
#add end


cp nvidia-settings %{buildroot}/usr/bin/
cp libGL.la %{buildroot}/usr/lib64/
cp nvidia.icd %{buildroot}/etc/OpenCL/vendors/
cp monitoring.conf %{buildroot}/usr/share/nvidia/
cp nvidia-modprobe %{buildroot}/usr/bin/
cp nvidia-application-profiles-%{version}-key-documentation %{buildroot}/usr/share/nvidia/
cp nvidia-application-profiles-%{version}-rc                %{buildroot}/usr/share/nvidia/
cp nvidia-debugdump   %{buildroot}/usr/bin/
cp nvidia-cuda-mps-control %{buildroot}/usr/bin/
cp nvidia-bug-report.sh  %{buildroot}/usr/bin/
cp LICENSE               %{buildroot}/usr/share/doc/NVIDIA_GLX-1.0/
cp nvidia-settings.png   %{buildroot}/usr/share/doc/NVIDIA_GLX-1.0/
cp nvidia-installer      %{buildroot}/usr/bin/
cp pci.ids               %{buildroot}/usr/share/nvidia/
cp nvidia-cuda-mps-server %{buildroot}/usr/bin/
cp nvidia-smi        %{buildroot}/usr/bin/
cp nvidia-xconfig    %{buildroot}/usr/bin/
cp nvidia-settings.desktop  %{buildroot}/usr/share/applications/
cp nvidia-persistenced  %{buildroot}/usr/bin/

#libpath
cp libnvidia-gtk3.so.%{version} %{buildroot}/usr/lib64/
cp libnvcuvid.so.%{version} %{buildroot}/usr/lib64/
cp libnvidia-ifr.so.%{version} %{buildroot}/usr/lib64/
cp libnvidia-gtk2.so.%{version} %{buildroot}/usr/lib64/
cp libvdpau_nvidia.so.%{version} %{buildroot}/usr/lib64/vdpau/
#libvdpau.so 和libvdpau-devel 冲突
cp libvdpau_nvidia.so.%{version} %{buildroot}/usr/lib64/ 
#cp libvdpau_trace.so.%{version}  %{buildroot}/usr/lib64/vdpau
cp libnvidia-tls.so.%{version} %{buildroot}/usr/lib64/
cp libnvidia-glsi.so.%{version} %{buildroot}/usr/lib64/
cp libnvidia-cfg.so.%{version} %{buildroot}/usr/lib64/
cp libnvidia-glcore.so.%{version} %{buildroot}/usr/lib64/
cp libnvidia-wfb.so.%{version} %{buildroot}/usr/lib64/nvidia/xorg/modules/
cp libglx.so.%{version}        %{buildroot}/usr/lib64/nvidia/xorg/modules/extensions/
cp libnvidia-eglcore.so.%{version} %{buildroot}/usr/lib64/
#remove not used lib 
cp libcuda.so.%{version} %{buildroot}/usr/lib64/ 
#cp libEGL.so.%{version} %{buildroot}/usr/lib64/
cp nvidia_drv.so   %{buildroot}/usr/lib64/nvidia/xorg/modules/drivers/
cp libnvidia-compiler.so.%{version} %{buildroot}/usr/lib64/
cp libnvidia-fbc.so.%{version} %{buildroot}/usr/lib64/
cp ./tls/libnvidia-tls.so.%{version} %{buildroot}/usr/lib64/mytls/
cp libnvidia-encode.so.%{version} %{buildroot}/usr/lib64/
#change for rhel7.3
#cp libGLESv2.so.%{version} %{buildroot}/usr/lib64/
cp libnvidia-ml.so.%{version} %{buildroot}/usr/lib64/
cp libGL.so.%{version} %{buildroot}/usr/lib64/
cp libnvidia-opencl.so.%{version} %{buildroot}/usr/lib64/
cp libOpenCL.so.1.0.0 %{buildroot}/usr/lib64/
#cp libGLESv1_CM.so.%{version} %{buildroot}/usr/lib64/

############################ add by rhel 7.3 ##################
cp libGL.so.1.0.0  %{buildroot}/usr/lib64/
cp libGLdispatch.so.0   %{buildroot}/usr/lib64/
# file /usr/lib64/libEGL.so.1 from install of nvidia-367.44-rhel7.3.x86_64 conflicts with file from package mesa-libEGL-11.2.2-2.20160614.el7.x86_64
#cp libEGL.so.1    %{buildroot}/usr/lib64/                                
cp libEGL_nvidia.so.%{version}    %{buildroot}/usr/lib64/ 
#need to check 
cp libGLESv1_CM.so.1   %{buildroot}/usr/lib64/ 
cp libGLESv1_CM_nvidia.so.%{version}      %{buildroot}/usr/lib64/ 
cp libGLESv2.so.2    %{buildroot}/usr/lib64/
cp libGLESv2_nvidia.so.%{version}    %{buildroot}/usr/lib64/
cp libGLX.so.0  %{buildroot}/usr/lib64/
cp libGLX_nvidia.so.%{version} %{buildroot}/usr/lib64/
#cp libnvidia-egl-wayland.so.%{version}   %{buildroot}/usr/lib64/
cp libnvidia-fatbinaryloader.so.%{version}  %{buildroot}/usr/lib64/
cp libnvidia-ptxjitcompiler.so.%{version}  %{buildroot}/usr/lib64/
cp libOpenGL.so.0  %{buildroot}/usr/lib64/ 
cp nvidia_icd.json   %{buildroot}/etc/vulkan/icd.d/

####################  add rhel7.3 end  ###########################

# blacklist
mkdir -p %{buildroot}/etc/modprobe.d/
cp %{SOURCE1} %{buildroot}/etc/modprobe.d/
cp %{SOURCE2} %{buildroot}/etc/X11/
cp %{SOURCE3} %{buildroot}/usr/share/gdm/greeter/autostart/

%clean
#rm -rf $RPM_BUILD_ROOT

%post
#backup the conflict
if [ -f /usr/lib64/xorg/modules/libglamoregl.so ]
then 
    if [ ! -f /usr/lib64/xorg/modules/libglamoregl.so.bak ]
    then
        cp /usr/lib64/xorg/modules/libglamoregl.so /usr/lib64/xorg/modules/libglamoregl.so.bak
    fi
fi

if [ -f /usr/lib64/xorg/modules/extensions/libglx.so ]
then 
    if [ ! -f /usr/lib64/xorg/modules/extensions/libglx.so.bak ]
    then
        cp  /usr/lib64/xorg/modules/extensions/libglx.so /usr/lib64/xorg/modules/extensions/libglx.so.bak
    fi
fi

mkdir -p /usr/lib64/tls/
cp /usr/lib64/mytls/* /usr/lib64/tls/
ln -sf %{_libdir}/libnvcuvid.so.%{version} %{_libdir}/libnvcuvid.so.1
ln -sf %{_libdir}/libnvcuvid.so.1 %{_libdir}/libnvcuvid.so

ln -sf %{_libdir}/libnvidia-ifr.so.%{version} %{_libdir}/libnvidia-ifr.so.1
ln -sf %{_libdir}/libnvidia-ifr.so.1 %{_libdir}/libnvidia-ifr.so

ln -sf %{_libdir}/vdpau/libvdpau_nvidia.so.%{version} %{_libdir}/vdpau/libvdpau_nvidia.so.1
ln -sf %{_libdir}/vdpau/libvdpau_nvidia.so.%{version} %{_libdir}/libvdpau_nvidia.so

#libvdpau.so 20150528 
# ?? need to check 
ln -sf %{_libdir}/libvdpau_nvidia.so.%{version}   %{_libdir}/libvdpau_nvidia.so.1
ln -sf %{_libdir}/libvdpau_nvidia.so.1            %{_libdir}/libvdpau_nvidia.so

#libvdpau_trace.so 20150528
#ln -sf %{_libdir}/vdpau/libvdpau_trace.so.%{version}   %{_libdir}/vdpau/libvdpau_trace.so.1
#ln -sf %{_libdir}/vdpau/libvdpau_trace.so.%{version}   %{_libdir}/libvdpau_trace.so

ln -sf %{_libdir}/libnvidia-cfg.so.%{version} %{_libdir}/libnvidia-cfg.so.1
ln -sf %{_libdir}/libnvidia-cfg.so.1 %{_libdir}/libnvidia-cfg.so

#20150528.modify
ln -sf %{_libdir}/nvidia/xorg/modules/libnvidia-wfb.so.%{version} %{_libdir}/xorg/modules/libnvidia-wfb.so.1

ln -sf %{_libdir}/nvidia/xorg/modules/extensions/libglx.so.%{version}  %{_libdir}/xorg/modules/extensions/libglx.so

ln -sf %{_libdir}/libcuda.so.%{version} %{_libdir}/libcuda.so.1
ln -sf %{_libdir}/libcuda.so.1          %{_libdir}/libcuda.so
#change for rhel7.3
#ln -sf %{_libdir}/libEGL.so.%{version} %{_libdir}/libEGL.so.1
#link the default lib in os to libEGL.so
ln -sf %{_libdir}/libEGL.so.1           %{_libdir}/libEGL.so

ln -sf %{_libdir}/libnvidia-fbc.so.%{version} %{_libdir}/libnvidia-fbc.so.1
ln -sf %{_libdir}/libnvidia-fbc.so.1          %{_libdir}/libnvidia-fbc.so

ln -sf %{_libdir}/libnvidia-encode.so.%{version}  %{_libdir}/libnvidia-encode.so.1
ln -sf %{_libdir}/libnvidia-encode.so.1           %{_libdir}/libnvidia-encode.so

#change for rhel7.3
#ln -sf %{_libdir}/libGLESv2.so.%{version} %{_libdir}/libGLESv2.so.2
ln -sf %{_libdir}/libGLESv2.so.2 %{_libdir}/libGLESv2.so

ln -sf %{_libdir}/libnvidia-ml.so.%{version} %{_libdir}/libnvidia-ml.so.1
ln -sf %{_libdir}/libnvidia-ml.so.1 %{_libdir}/libnvidia-ml.so
#this lib is not used .
#ln -sf %{_libdir}/libGL.so.%{version} %{_libdir}/libGL.so.1
#ln -sf %{_libdir}/libGL.so.1 %{_libdir}/libGL.so

ln -sf %{_libdir}/libnvidia-opencl.so.%{version} %{_libdir}/libnvidia-opencl.so.1

ln -sf %{_libdir}/libOpenCL.so.1.0.0 %{_libdir}/libOpenCL.so.1.0
ln -sf %{_libdir}/libOpenCL.so.1.0 %{_libdir}/libOpenCL.so.1
ln -sf %{_libdir}/libOpenCL.so.1 %{_libdir}/libOpenCL.so
#masked  for rhel7.3 
#ln -sf %{_libdir}/libGLESv1_CM.so.%{version} %{_libdir}/libGLESv1_CM.so.1
ln -sf %{_libdir}/libGLESv1_CM.so.1 %{_libdir}/libGLESv1_CM.so

##########add for rhel 7.3#################
ln -sf %{_libdir}/libGLdispatch.so.0   %{_libdir}/libGLdispatch.so

ln -sf %{_libdir}/libEGL_nvidia.so.%{version}  %{_libdir}/libEGL_nvidia.so.0
ln -sf %{_libdir}/libEGL_nvidia.so.0  %{_libdir}/libEGL_nvidia.so

ln -sf %{_libdir}/libGLESv1_CM_nvidia.so.%{version}  %{_libdir}/libGLESv1_CM_nvidia.so.1
ln -sf %{_libdir}/libGLESv1_CM_nvidia.so.1  %{_libdir}/libGLESv1_CM_nvidia.so

ln -sf %{_libdir}/libGLESv2_nvidia.so.%{version}    %{_libdir}/libGLESv2_nvidia.so.2
ln -sf %{_libdir}/libGLESv2_nvidia.so.2   %{_libdir}/libGLESv2_nvidia.so

#tmp removed 
ln -sf  %{_libdir}/libOpenGL.so.0     %{_libdir}/libOpenGL.so

ln -sf %{_libdir}/libGL.so.1.0.0  %{_libdir}/libGL.so.1
ln -sf %{_libdir}/libGL.so.1  %{_libdir}/libGL.so

ln -sf %{_libdir}/nvidia/xorg/modules/drivers/nvidia_drv.so  %{_libdir}/xorg/modules/drivers/nvidia_drv.so
##########add end##########################

#mv /usr/lib64/xorg/modules/libglamoregl.so /usr/lib64/xorg/modules/libglamoregl.so.old

#20150529 privilege right
/usr/bin/execstack -c /usr/lib64/libnvidia-compiler.so.%{version}
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libnvidia-compiler.so.%{version}
/usr/bin/execstack -c /usr/lib64/libnvidia-eglcore.so.%{version}
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libnvidia-eglcore.so.%{version}
/usr/bin/execstack -c /usr/lib64/libnvidia-glsi.so.%{version}
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libnvidia-glsi.so.%{version}

#masked for rhel7.3
#/usr/bin/execstack -c /usr/lib64/libGLESv2.so.%{version}
#/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libGLESv2.so.%{version}
#/usr/bin/execstack -c /usr/lib64/libGLESv1_CM.so.%{version}
#/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libGLESv1_CM.so.%{version}

#remove %{version} add so.1 
#/usr/bin/execstack -c /usr/lib64/libEGL.so.%{version}
#/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libEGL.so.%{version}
/usr/bin/execstack -c /usr/lib64/libEGL.so.1
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libEGL.so.1
# add libGLESv2_nvidia.so
/usr/bin/execstack -c /usr/lib64/libGLESv2_nvidia.so.%{version}
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libGLESv2_nvidia.so.%{version}
#add libOpenGL.so.0.
/usr/bin/execstack -c /usr/lib64/libOpenGL.so.0
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libOpenGL.so.0
#add for wayland 
#/usr/bin/execstack -c /usr/lib64/libnvidia-egl-wayland.so.%{version}
#/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libnvidia-egl-wayland.so.%{version}

/usr/bin/execstack -c /usr/lib64/libnvidia-encode.so.%{version}
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libnvidia-encode.so.%{version}
/usr/bin/execstack -c /usr/lib64/libcuda.so.%{version}
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libcuda.so.%{version}
/usr/bin/execstack -c /usr/lib64/libnvidia-opencl.so.%{version}
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libnvidia-opencl.so.%{version}
/usr/bin/execstack -c /usr/lib64/libOpenCL.so.1.0.0
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libOpenCL.so.1.0.0
/usr/bin/execstack -c /usr/lib64/libnvidia-cfg.so.%{version}
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libnvidia-cfg.so.%{version}
/usr/bin/execstack -c /usr/lib64/libnvcuvid.so.%{version}
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libnvcuvid.so.%{version}
/usr/bin/execstack -c /usr/lib64/libnvidia-fbc.so.%{version}
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libnvidia-fbc.so.%{version}
/usr/bin/execstack -c /usr/lib64/libnvidia-ifr.so.%{version}
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libnvidia-ifr.so.%{version}
/usr/bin/execstack -c /usr/lib64/libnvidia-ml.so.%{version}
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libnvidia-ml.so.%{version}
/usr/bin/execstack -c /usr/lib64/libnvidia-glcore.so.%{version}
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libnvidia-glcore.so.%{version}
#/usr/bin/execstack -c /usr/lib64/libGL.so.%{version}
#/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libGL.so.%{version}
#/usr/bin/execstack -c /usr/lib64/libGL.so.1.0.0
/usr/bin/execstack -c /usr/lib64/nvidia/xorg/modules/extensions/libglx.so.%{version}
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/nvidia/xorg/modules/extensions/libglx.so.%{version}
/usr/bin/execstack -c /usr/lib64/libnvidia-tls.so.%{version}
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libnvidia-tls.so.%{version}
/usr/bin/execstack -c /usr/lib64/tls/libnvidia-tls.so.%{version}
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/tls/libnvidia-tls.so.%{version}
#/usr/bin/execstack -c /usr/lib64/libvdpau.so.%{version}
#/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libvdpau.so.%{version}
#/usr/bin/execstack -c /usr/lib64/vdpau/libvdpau_trace.so.%{version}
#/usr/bin/chcon -t textrel_shlib_t /usr/lib64/vdpau/libvdpau_trace.so.%{version}
/usr/bin/execstack -c /usr/lib64/vdpau/libvdpau_nvidia.so.%{version}
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/vdpau/libvdpau_nvidia.so.%{version}
/usr/bin/execstack -c /usr/lib64/nvidia/xorg//modules/drivers/nvidia_drv.so
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/nvidia/xorg/modules/drivers/nvidia_drv.so
/usr/bin/execstack -c /usr/lib64/nvidia/xorg/modules/libnvidia-wfb.so.%{version}
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/nvidia/xorg/modules/libnvidia-wfb.so.%{version}
/usr/bin/execstack -c /usr/lib64/libnvidia-gtk2.so.%{version}
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libnvidia-gtk2.so.%{version}
/usr/bin/execstack -c /usr/lib64/libnvidia-gtk3.so.%{version}
/usr/bin/chcon -t textrel_shlib_t /usr/lib64/libnvidia-gtk3.so.%{version}
/usr/sbin/ldconfig


#/usr/sbin/depmod -a

#DMKS_VERSION
#dkms add -m %{dkms_name} -v %{version} -q || :
# Rebuild and make available for the currently running kernel
#dkms build -m %{dkms_name} -v %{version} -q || :
#dkms install -m %{dkms_name} -v %{version} -q --force || :
#add by perry start
# if [ -f /usr/src/%{module_name}-%{version} ]
# then 
	# dkms remove -m %{dkms_name} -v %{version} --all
	#rm -rf /usr/src/%{module_name}-%{version}
# done

#add by perry end 
dkms add -m %{module_name} -v %{version}
for kernel in /boot/config-*; do
	KERNEL=${kernel#*-}
	dkms build -m %{module_name} -v %{version} -k ${KERNEL}
	dkms install -m %{module_name} -v %{version} -k ${KERNEL}
	depmod -aq ${KERNEL}
	dracut --host-only --kver $KERNEL -f
done
#remove to avoid creating some wrong xorg config parameters 
#/usr/bin/nvidia-xconfig


%postun
# remove link 
#dkms remove -m %{dkms_name} -v %{version} -q --all || :
dkms remove -m %{dkms_name} -v %{version} --all
rm -f /lib/modules/`uname -r`/kernel/drivers/video/nvidia.ko
rm -rf /usr/src/%{module_name}-%{version}
rm -rf /lib/modules/`uname -r`/extra/nvidia*.ko
depmod -a
rm -f /usr/lib64/libnvcuvid.so.1
rm -f /usr/lib64/libnvcuvid.so
rm -f /usr/lib64/libnvidia-ifr.so.1
rm -f /usr/lib64/libnvidia-ifr.so
rm -f /usr/lib64/libvdpau_nvidia.so
rm -f /usr/lib64/libnvidia-cfg.so.1
rm -f /usr/lib64/libnvidia-cfg.so
rm -f /usr/lib64/libcuda.so.1
rm -f /usr/lib64/libcuda.so
rm -f /usr/lib64/libEGL.so.1
rm -f /usr/lib64/libnvidia-fbc.so.1
rm -f /usr/lib64/libEGL.so
rm -f /usr/lib64/libnvidia-fbc.so
rm -f /usr/lib64/libnvidia-encode.so.1
rm -f /usr/lib64/libnvidia-encode.so
rm -f /usr/lib64/libGLESv2.so.2
rm -f /usr/lib64/libGLESv2.so
rm -f /usr/lib64/libnvidia-ml.so.1
rm -f /usr/lib64/libnvidia-ml.so
rm -f /usr/lib64/libOpenCL.so.1.0
rm -f /usr/lib64/libnvidia-opencl.so.1
rm -f /usr/lib64/libOpenCL.so.1
rm -f /usr/lib64/libOpenCL.so
rm -f /usr/lib64/libGLESv1_CM.so.1
rm -f /usr/lib64/libGLESv1_CM.so
#add for rhel7.3 start 
rm -rf /usr/lib64/nvidia/                                 
rm -f /usr/lib64/libEGL.so.1                                 
rm -f /usr/lib64/libGL.so.1.0.0
rm -f /usr/lib64/libGLdispatch.so.0
rm -f /usr/lib64/libEGL_nvidia.so.%{version}
rm -f /usr/lib64/libGLESv1_CM_nvidia.so.%{version}

rm -f /usr/lib64/libGLESv2_nvidia.so.%{version}
#rm -f /usr/lib64/libGLX.so.0
rm -f /usr/lib64/libGLX_nvidia.so.%{version}
rm -f /usr/lib64/libnvidia-egl-wayland.so.%{version}
rm -f /usr/lib64/libnvidia-fatbinaryloader.so.%{version}
rm -f /usr/lib64/libnvidia-ptxjitcompiler.so.%{version}
rm -f /usr/lib64/libOpenGL.so.0
rm -f /etc/vulkan/icd.d/nvidia_icd.json
rm -f /usr/lib64/nvidia-application-profiles-%{version}-rc 
rm -f /usr/share/nvidia/nvidia-application-profiles-%{version}-key-documentation
rm -f /usr/share/nvidia/nvidia-application-profiles-%{version}-rc
rm -f /usr/share/gdm/greeter/autostart/prime.desktop
rm -rf /var/lib/dkms/nvidia/
#end 

#20150528
#rm -f  %{_libdir}/libvdpau_trace.so
#ln -sf %{_libdir}/vdpau/libvdpau_trace.so.1.0.0   %{_libdir}/vdpau/libvdpau_trace.so.1

rm -f  %{_libdir}/xorg/modules/libnvidia-wfb.so.1
rm -f  %{_libdir}/nvidia/
#remove from rhel7.3
#ln -sf %{_libdir}/libvdpau.so.1.0.0  %{_libdir}/libvdpau.so
#rm -f  %{_libdir}/libvdpau.so.1

ln -sf %{_libdir}/libGL.so.1.2.0     %{_libdir}/libGL.so.1
cp -f /usr/lib64/xorg/modules/libglamoregl.so.bak /usr/lib64/xorg/modules/libglamoregl.so
rm -f /usr/lib64/xorg/modules/extensions/libglx.so
cp /usr/lib64/xorg/modules/extensions/libglx.so.bak /usr/lib64/xorg/modules/extensions/libglx.so
ln -s /usr/lib64/libEGL.so.1.0.0 /usr/lib64/libEGL.so.1

%files
%defattr(0777,root,root,0644)
#NVIDIA DKMS_VESION
%{_usrsrc}/*
%{_bindir}/*
%{_libdir}/*
/usr/share/applications/nvidia-settings.desktop
/usr/share/doc/NVIDIA_GLX-1.0/LICENSE
/usr/share/doc/NVIDIA_GLX-1.0/nvidia-settings.png
/usr/share/nvidia/monitoring.conf
/usr/share/nvidia/nvidia-application-profiles-%{version}-key-documentation
/usr/share/nvidia/nvidia-application-profiles-%{version}-rc
/usr/share/nvidia/pci.ids
/etc/modprobe.d/blacklist-nouveau.conf
/etc/X11/xorg.conf
/etc/OpenCL/vendors/nvidia.icd
/etc/vulkan/icd.d/nvidia_icd.json
/usr/share/gdm/greeter/autostart/prime.desktop
#/lib/modules/%{_kmodver}/kernel/drivers/video/nvidia.ko

%changelog

* Wed Feb 29 2012 benLG<liben.guo@cs2c.com.cn> - 290.10-k3253.1
- Rebuild for 3.2.5-3 kernel.

* Fri Oct 28 2011 zhangwei<wei.zhang@cs2c.com.cn> - 285.05.09-k38.1
- rebuild nvidia driver v285.05.09 in kernel 2.6.38.8-35.3.nk
