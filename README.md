# MarFS
MarFS provides a scalable near-POSIX file system by using one or more POSIX file systems as a scalable metadata component and one or more data stores (object, file, etc) as a scalable data component.

Our default implementation uses GPFS file systems as the metadata component and multiple ZFS instances, exported over NFS, as the data component.

The MetaData Abstraction Layer and Data Abstraction Layer (MDAL/DAL) provide a modular way to introduce alternative implementations (metadata and data implementations, respectively).
The DAL is provided by LibNE, which also adds the ability to use our own erasure + checksumming implementation, utilizing Intel's Storage Acceleration Library ( "https://github.com/intel/isa-l" ).

The following additional repo is required to build MarFS:
  - Intel's Storage Acceleration Library
    - Location : "https://github.com/intel/isa-l"

The following additional repo is recommended:
  - Pftool ( Parallel File Tool )
    - A highly performant data transfer utilitiy with special MarFS integration
    - Location : "https://github.com/pftool/pftool"

## Documentation

The most up-to-date documentation is here: http://mar-file-system.github.io/marfs/

There is also extensive documentation in the Documents directory, including theory and older install guides.

