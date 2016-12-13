# What is the nvidia-run-to-rpm ?

The project is created for making NVIDIA close source rpm package which can be installed on RHEL or Centos and others basing on RPM .
It includes the NVIDIA dkms driver,even you  update your kernel version ,the dkms will build new NVIDIA driver basing on the new kernel source codes .


  
#NVIDIA Driver version added:

1.NVIDIA-Linux-x86-367.44  added 2016.11

2.NVIDIA-Linux-x86-370.28  added 2016.12

# How to build :
1.
	copy NVIDIA-Linux-x86-367.44.tar.gz to your rombuild directory ,
	for example :/home/perry/rpmbuild/SOURCE/ ,and then tar xzvf NVIDIA-Linux-x86-367.44.tar.gz to extract it .

2.
	copy below files to "/home/perry/rpmbuild/SOURCE/"
	blacklist-nouveau.conf	 
	prime.desktop	 
	xorg.conf

3.
	copy below file to "/home/perry/rpmbuild/SPEC/"
	nvidia-driver-x64.spec	 

4.
	install rpmbuild dependency packages .

5.
	cd /home/perry/rpmbuild/
	run "rpmbuild -ba SPEC/nvidia-driver-x64.spec"

