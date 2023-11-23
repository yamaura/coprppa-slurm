.PHONY: srpm rpm

srpm: /usr/bin/spectool
	spectool -g $(spec)
	rpmbuild -bs $(spec) --define "_sourcedir $(PWD)" --define "_srcrpmdir $(outdir)"

rpm: srpm
	rpmbuild --rebuild *.src.rpm --define "_sourcedir $(PWD)" --define "_rpmdir $(PWD)"

mock: srpm
	mock --rebuild *.src.rpm

/usr/bin/spectool:
	dnf install -y rpmdevtools
