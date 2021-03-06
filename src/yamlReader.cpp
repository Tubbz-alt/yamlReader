#include <yamlReader.h>
#include <iostream>
#include <algorithm>

static Hub buildHier(const char *ip_addr)
{
    NetIODev      commDev     = INetIODev::create("udp",  ip_addr);
    MMIODev       mmio        = IMMIODev::create("mmio", 0x80000000, LE );
    AxiMicronN25Q prom        = IAxiMicronN25Q::create("prom");
    IntField      tarballAddr = IIntField::create("CPSW_TARBALL_ADDR_C");

    ProtoStackBuilder bldr = IProtoStackBuilder::create();
    bldr->setSRPVersion(IProtoStackBuilder::SRP_UDP_V2);
    bldr->setUdpPort(8192);

    commDev->addAtAddress( mmio, bldr );

    mmio->addAtAddress( tarballAddr, 0x00000404 );
    mmio->addAtAddress( prom, 0x02000000 );

    return commDev;
}

IYamlReaderImpl::IYamlReaderImpl( const std::string& ipAddr, int verbosity )
:
    dirName( "." ),
    fileName( "yaml.tar.gz" ),
    outputFile( dirName + "/" + fileName ),
    dfltVerbosity( verbosity ),
    ostream( NULL )
{
    if ( dfltVerbosity < YAML_READER_TO_STDOUT || dfltVerbosity > YAML_READER_TO_STDERR )
    {
        dfltVerbosity = YAML_READER_TO_STDOUT;
    }
    try
    {
        Hub root               = buildHier( ipAddr.c_str() );
        ScalVal_RO tarballAddr = IScalVal_RO::create( root->findByName( "mmio/CPSW_TARBALL_ADDR_C" ) );
        prom                   = IEEProm::create( root->findByName( "mmio/prom" ) );

        tarballAddr->getVal(&startAddress);

        chat( dfltVerbosity, "Tarball start address found: 0x%08X\n", startAddress );
    }
    catch (CPSWError e)
    {
        fprintf( stderr, "CPSW Error during YamlReader object construction: %s\n",  e.getInfo().c_str() );
        throw;
    }
}

void IYamlReaderImpl::setOutputDir( const std::string& dir )
{
    dirName = dir;
    outputFile = dirName + "/" + fileName;
}

void IYamlReaderImpl::setFileName(  const std::string& name )
{
    fileName = name;
    if ( fileName.empty() )
    {
        outputFile.clear();
        ostream = &std::cout;
    }
    else
    {
        outputFile = dirName + "/" + fileName;
        ostream = NULL;
    }
}

void IYamlReaderImpl::setOutputStream( std::ostream *strm )
{
    ostream = strm;
}

void IYamlReaderImpl::chat(int verbosity, const char *fmt, ...) const
{
va_list ap;

    va_start(ap, fmt);

    if ( YAML_READER_DFLT_VERB == verbosity )
    {
        verbosity = dfltVerbosity;
    }

    if ( verbosity != YAML_READER_QUIET )
    {
        FILE *f = YAML_READER_TO_STDERR == verbosity ?  ::stderr : ::stdout;
    
        vfprintf(f, fmt, ap);
    }

    va_end(ap);
}

void IYamlReaderImpl::readTarball(int verb)
{
    if ( ostream )
    {
        readTarball( ostream, verb );
    }
    else
    {
        RAIIFile file( outputFile, std::ios_base::out | std::ios_base::binary );
        readTarball( file.f(), verb );
   		chat( verb, "The tarball was written to %s\n",   outputFile.c_str() );
    }
}

void IYamlReaderImpl::readTarball(std::ostream *fp, int verb)
{
    chat( verb, "Starting reading of the tarball file from the PROM...\n" );
    try
    {
        std::vector<uint32_t> raw_data(mem_block_size, 0);
        bool                  first_loop = true;
        uint32_t              addr       = startAddress;
        uint32_t              tail_val;

        // Setup for 32-bit address mode
        prom->setAddr32BitMode( true );

        while( 1 )
        {
            prom->readProm( addr, raw_data.data() );

            // On first loop, verify if it is a valid GZ file
            if ( first_loop )
            {
                uint16_t header = ( (raw_data.at(0) >> 16) & 0xffff );

                if ( header != 0x1f8b )
                    throw std::runtime_error( "Invalid GZIP header. Aborting." );
            }

            // Look for the end of file in memrory, marked by the start of memory block set to 0xffffffff.
            //
            // The last valid word (the one right before the marker), could contain 0xff bytes in its tail
            // which must be removed before writing the word to the output file.
            //
            // As the marker can be found at the begin of a memory block, we must keep the last word of a block
            // until the next block is read; if the block start with the marker then this word was the last one
            // of the file, and must be check for tailing 0xff bytes.
            std::vector<uint32_t>::iterator it = std::find(raw_data.begin(), raw_data.end(), 0xffffffff);


            // Process the block if it does not start with the marker.
            //
            // The first memory block (when first_loop is true) can not be the marker as we at this point
            // already check the GZ header in that location.
            if (it != raw_data.begin())
            {
                // At this point, the block does not start with the marker. So, copy the saved last word of the previous
                // memory block to the oputput file. But, only after the first loop.
                if ( ! first_loop )
                    copyWord( tail_val, fp, false );

                // At this point we are done with the first loop, so we set this flag to  false, and it is never set
                // to true again.
                first_loop = false;

                // Make a copy of the last word od the block, and copy all the rest the output file.
                tail_val = *(it-1);
                for (std::vector<uint32_t>::const_iterator k = raw_data.begin();
                     k != it-1;
                     ++k)
                {
                    copyWord( *k, fp, false );
                }

                // If the marker was not found in this block, continue reading a new block.
                if (it == raw_data.end())
                {
                    addr += mem_block_byte_size;
                    continue;
                }
            }

            // We get here is the marker was found in the current memory block. Write the last word, removing
            // trailing 0xff bytes, update the endAddress and return.

            copyWord( tail_val, fp, true );

            endAddress = addr;

    		chat( verb, "A valid tarball was found:\n" );
    		chat( verb, "   Start address     : 0x%08X\n",   startAddress );
    		chat( verb, "   End address       : 0x%08X\n",   endAddress   );
    		chat( verb, "   Tarball file size : %d bytes\n", endAddress - startAddress  );
            return;
        }
    }
    catch ( CPSWError e )
    {
        fprintf( stderr,"CPSW Error during PROM reading: %s\n", e.getInfo().c_str() );
        throw;
    }
}

void IYamlReaderImpl::untar( const bool stripRootDir ) const
{
int status;

    if ( outputFile.empty() ) {
    	throw CPSWError("Unable to untar without a file name");
    }

    std::string cmd = "tar -zxf " + outputFile + " -C " + dirName;

    if ( stripRootDir )
        cmd += " --strip-components=1";

    if ( (status = system( cmd.c_str() )) )
    {
    	fprintf(stderr,"system(\"%s\") FAILED -- return status %i\n", cmd.c_str(), status);
    	throw IOError("system(\"tar -zxf\") failed");
    }

    chat( dfltVerbosity, "The tarball was ungziped and untared on %s\n",  dirName.c_str() );

    if ( stripRootDir )
        chat( dfltVerbosity, "The root directory in the tarball was stripped\n" );

}

void IYamlReaderImpl::copyWord( const uint32_t& u32, std::ostream* file, bool trim_tail )
{
    std::vector<uint8_t> u8(4,0);
    *(reinterpret_cast<uint32_t*>(u8.data())) = htobe32(u32);

    if ( trim_tail )
    {
        for ( std::vector<uint8_t>::reverse_iterator it = u8.rbegin(); it != u8.rend(); ++it )
        {
            if (*it == 0xff)
            {
                *it = 0x00;
            }
            else
            {
                if (it == u8.rbegin())
                    u8.push_back(0);
                break;
            }
        }
    }

    std::copy(u8.begin(), u8.end(), std::ostream_iterator<uint8_t>(*file));
}

YamlReader IYamlReader::create( const std::string& ipAddr, int verb )
{
    return YamlReader( cpsw::make_shared<IYamlReaderImpl>( ipAddr, verb ) );
}
