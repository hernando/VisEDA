
/* Copyright (c) 2014-2016, Human Brain Project
 *                          Daniel Nachbaur <daniel.nachbaur@epfl.ch>
 *                          Stefan.Eilemann@epfl.ch
 */

#include "subscriber.h"

#include "event.h"
#include "log.h"
#include "detail/broker.h"
#include "detail/constants.h"
#include "detail/sender.h"
#include "detail/socket.h"
#include "detail/byteswap.h"

#include <servus/serializable.h>
#include <servus/servus.h>

#include <cassert>
#include <cstring>
#include <map>
#include <stdexcept>

namespace zeq
{
class Subscriber::Impl
{
public:
    Impl( const std::string& session, void* context )
        : _browser( PUBLISHER_SERVICE )
        , _selfInstance( detail::Sender::getUUID( ))
        , _session( session == DEFAULT_SESSION ? getDefaultSession() : session )
    {
        if( _session == zeq::NULL_SESSION || session.empty( ))
            ZEQTHROW( std::runtime_error( std::string(
                    "Invalid session name for subscriber" )));

        if( !servus::Servus::isAvailable( ))
            ZEQTHROW( std::runtime_error(
                          std::string( "Empty servus implementation" )));

        _browser.beginBrowsing( servus::Servus::IF_ALL );
        update( context );
    }

    Impl( const URI& uri, void* context )
        : _browser( PUBLISHER_SERVICE )
        , _selfInstance( detail::Sender::getUUID( ))
    {
        if( uri.getHost().empty() || uri.getPort() == 0 )
                ZEQTHROW( std::runtime_error( std::string(
                              "Non-fully qualified URI used for subscriber" )));

        const std::string& zmqURI = buildZmqURI( uri );
        if( !addConnection( context, zmqURI, uint128_t( )))
        {
            ZEQTHROW( std::runtime_error(
                          "Cannot connect subscriber to " + zmqURI + ": " +
                           zmq_strerror( zmq_errno( ))));
        }
    }

    Impl( const URI& uri, const std::string& session, void* context )
        : _browser( PUBLISHER_SERVICE )
        , _selfInstance( detail::Sender::getUUID( ))
        , _session( session == DEFAULT_SESSION ? getDefaultSession() : session )
    {
        if( _session == zeq::NULL_SESSION || session.empty( ))
            ZEQTHROW( std::runtime_error( std::string(
                    "Invalid session name for subscriber" )));

        if( uri.getHost().empty() || uri.getPort() == 0 )
        {
            if( !servus::Servus::isAvailable( ))
                ZEQTHROW( std::runtime_error(
                              std::string( "Empty servus implementation" )));

            _browser.beginBrowsing( servus::Servus::IF_ALL );
            update( context );
        }
        else
        {
            const std::string& zmqURI = buildZmqURI( uri );
            if( !addConnection( context, zmqURI, uint128_t( )))
            {
                ZEQTHROW( std::runtime_error(
                              "Cannot connect subscriber to " + zmqURI + ": " +
                               zmq_strerror( zmq_errno( ))));
            }
        }
    }

    ~Impl()
    {
        for( const auto& socket : _subscribers )
        {
            if( socket.second )
                zmq_close( socket.second );
        }
        if( _browser.isBrowsing( ))
            _browser.endBrowsing();
    }

    bool registerHandler( const uint128_t& event, const EventFunc& func )
    {
        if( _eventFuncs.count( event ) != 0 )
            return false;

        // Add subscription to existing sockets
        for( const auto& socket : _subscribers )
        {
            if( socket.second &&
                zmq_setsockopt( socket.second, ZMQ_SUBSCRIBE,
                                &event, sizeof( event )) == -1 )
            {
                ZEQTHROW( std::runtime_error(
                    std::string( "Cannot update topic filter: " ) +
                    zmq_strerror( zmq_errno( ))));
            }
        }

        _eventFuncs[event] = func;
        return true;
    }

    bool deregisterHandler( const uint128_t& event )
    {
        if( _eventFuncs.erase( event ) == 0 )
            return false;

        for( const auto& socket : _subscribers )
        {
            if( socket.second &&
                zmq_setsockopt( socket.second, ZMQ_UNSUBSCRIBE,
                                &event, sizeof( event )) == -1 )
            {
                ZEQTHROW( std::runtime_error(
                    std::string( "Cannot update topic filter: " ) +
                    zmq_strerror( zmq_errno( ))));
            }
        }

        return true;
    }

    bool hasHandler( const uint128_t& event ) const
    {
        return _eventFuncs.count( event ) > 0;
    }

    bool subscribe( servus::Serializable& serializable )
    {
        const uint128_t& type = serializable.getTypeIdentifier();
        if( _serializables.count( type ) != 0 )
            return false;

        _subscribe( type );
        _serializables[ type ] = &serializable;
        return true;
    }

    bool unsubscribe( const servus::Serializable& serializable )
    {
        const uint128_t& type = serializable.getTypeIdentifier();
        if( _serializables.erase( type ) == 0 )
            return false;

        _unsubscribe( type );
        return true;
    }

    void addSockets( std::vector< detail::Socket >& entries )
    {
        entries.insert( entries.end(), _entries.begin(), _entries.end( ));
    }

    void process( detail::Socket& socket )
    {
        zmq_msg_t msg;
        zmq_msg_init( &msg );
        zmq_msg_recv( &msg, socket.socket, 0 );

        uint128_t type;
        memcpy( &type, zmq_msg_data( &msg ), sizeof(type) );
#ifndef COMMON_LITTLEENDIAN
        detail::byteswap( type ); // convert from little endian wire
#endif
        const bool payload = zmq_msg_more( &msg );
        zmq_msg_close( &msg );

        SerializableMap::const_iterator i = _serializables.find( type );
        if( i == _serializables.end( )) // FlatBuffer
        {
            zeq::Event event( type );
            if( payload )
            {
                zmq_msg_init( &msg );
                zmq_msg_recv( &msg, socket.socket, 0 );
                const size_t size = zmq_msg_size( &msg );
                ConstByteArray data( new uint8_t[size],
                                     std::default_delete< uint8_t[] >( ));
                memcpy( (void*)data.get(), zmq_msg_data( &msg ), size );
                event.setData( data, size );
                assert( event.getSize() == size );
                zmq_msg_close( &msg );
            }

            if( _eventFuncs.count( type ) != 0 )
                _eventFuncs[type]( event );
#ifndef NDEBUG
            else
            {
                // Note eile: The topic filtering in the handler registration
                // should ensure that we don't get messages we haven't
                // handlers. If this throws, something does not work.
                ZEQTHROW( std::runtime_error( "Got unsubscribed event" ));
            }
#endif
        }
        else // serializable
        {
            servus::Serializable* serializable = i->second;
            if( payload )
            {
                zmq_msg_init( &msg );
                zmq_msg_recv( &msg, socket.socket, 0 );
                serializable->fromBinary( zmq_msg_data( &msg ),
                                          zmq_msg_size( &msg ));
                zmq_msg_close( &msg );
            }
            serializable->notifyUpdated();
        }
    }

    void update( void* context )
    {
        if( _browser.isBrowsing( ))
            _browser.browse( 0 );
        const servus::Strings& instances = _browser.getInstances();
        for( const std::string& instance : instances )
        {
            const std::string& zmqURI = _getZmqURI( instance );

            // New subscription
            if( _subscribers.count( zmqURI ) == 0 )
            {
                const std::string& session = _browser.get( instance,
                                                           KEY_SESSION );
                if( _browser.containsKey( instance, KEY_SESSION ) &&
                    !_session.empty() && session != _session )
                {
                    continue;
                }

                const uint128_t identifier( _browser.get( instance,
                                                          KEY_INSTANCE ));
                if( !addConnection( context, zmqURI, identifier ))
                {
                    ZEQINFO << "Cannot connect subscriber to " << zmqURI << ": "
                            << zmq_strerror( zmq_errno( )) << std::endl;
                }
            }
        }
    }

    bool addConnection( void* context, const std::string& zmqURI,
                        const uint128_t& instance )
    {
        if( instance == _selfInstance )
            return true;

        _subscribers[zmqURI] = zmq_socket( context, ZMQ_SUB );

        if( zmq_connect( _subscribers[zmqURI], zmqURI.c_str( )) == -1 )
        {
            zmq_close( _subscribers[zmqURI] );
            _subscribers[zmqURI] = 0; // keep empty entry, unconnectable peer
            return false;
        }

        // Add existing subscriptions to socket
        for( const auto& i : _eventFuncs )
        {
            if( zmq_setsockopt( _subscribers[zmqURI], ZMQ_SUBSCRIBE,
                                &i.first, sizeof( uint128_t )) == -1 )
            {
                ZEQTHROW( std::runtime_error(
                    std::string( "Cannot update topic filter: " ) +
                    zmq_strerror( zmq_errno( ))));
            }
        }
        for( const auto& i : _serializables )
        {
            if( zmq_setsockopt( _subscribers[zmqURI], ZMQ_SUBSCRIBE,
                                &i.first, sizeof( uint128_t )) == -1 )
            {
                ZEQTHROW( std::runtime_error(
                    std::string( "Cannot update topic filter: " ) +
                    zmq_strerror( zmq_errno( ))));
            }
        }

        assert( _subscribers.find( zmqURI ) != _subscribers.end( ));
        if( _subscribers.find( zmqURI ) == _subscribers.end( ))
            return false;

        detail::Socket entry;
        entry.socket = _subscribers[zmqURI];
        entry.events = ZMQ_POLLIN;
        _entries.push_back( entry );
        ZEQINFO << "Subscribed to " << zmqURI << std::endl;
        return true;
    }

    const std::string& getSession() const { return _session; }

private:
    typedef std::map< uint128_t, EventFunc > EventFuncs;
    typedef std::map< std::string, void* > SocketMap;

    SocketMap _subscribers;
    EventFuncs _eventFuncs;

    typedef std::map< uint128_t, servus::Serializable* > SerializableMap;
    SerializableMap _serializables;

    servus::Servus _browser;
    std::vector< detail::Socket > _entries;

    const uint128_t _selfInstance;
    const std::string _session;

    std::string _getZmqURI( const std::string& instance )
    {
        const size_t pos = instance.find( ":" );
        const std::string& host = instance.substr( 0, pos );
        const std::string& port = instance.substr( pos + 1 );

        return buildZmqURI( DEFAULT_SCHEMA, host, std::stoi( port ));
    }

    void _subscribe( const uint128_t& event )
    {
        for( const auto& socket : _subscribers )
        {
            if( zmq_setsockopt( socket.second, ZMQ_SUBSCRIBE,
                                &event, sizeof( event )) == -1 )
            {
                ZEQTHROW( std::runtime_error(
                    std::string( "Cannot update topic filter: " ) +
                    zmq_strerror( zmq_errno( ))));
            }
        }
    }

    void _unsubscribe( const uint128_t& event )
    {
        for( const auto& socket : _subscribers )
        {
            if( zmq_setsockopt( socket.second, ZMQ_UNSUBSCRIBE,
                                &event, sizeof( event )) == -1 )
            {
                ZEQTHROW( std::runtime_error(
                    std::string( "Cannot update topic filter: " ) +
                    zmq_strerror( zmq_errno( ))));
            }
        }
    }
};

Subscriber::Subscriber()
    : Receiver()
    , _impl( new Impl( DEFAULT_SESSION, getZMQContext( )))
{
}

Subscriber::Subscriber( const std::string& session )
    : Receiver()
    , _impl( new Impl( session, getZMQContext( )))
{
}

Subscriber::Subscriber( const URI& uri )
    : Receiver()
    , _impl( new Impl( uri, getZMQContext( )))
{
}

Subscriber::Subscriber( const URI& uri, const std::string& session )
    : Receiver()
    , _impl( new Impl( uri, session, getZMQContext( )))
{
}

Subscriber::Subscriber( Receiver& shared )
    : Receiver( shared )
    , _impl( new Impl( DEFAULT_SESSION, getZMQContext( )))
{
}

Subscriber::Subscriber( const std::string& session, Receiver& shared )
    : Receiver( shared )
    , _impl( new Impl( session, getZMQContext( )))
{
}

Subscriber::Subscriber( const URI& uri, Receiver& shared  )
    : Receiver( shared )
    , _impl( new Impl( uri, getZMQContext( )))
{
}

Subscriber::Subscriber( const URI& uri, const std::string& session, Receiver& shared  )
    : Receiver( shared )
    , _impl( new Impl( uri, session, getZMQContext( )))
{
}

Subscriber::Subscriber( const servus::URI& uri )
    : Receiver()
    , _impl( new Impl( URI( uri ), DEFAULT_SESSION,
                                     getZMQContext( )))
{
    ZEQWARN << "zeq::Subscriber( const servus::URI& ) is deprecated"
            << std::endl;
}

Subscriber::Subscriber( const servus::URI& uri, Receiver& shared )
    : Receiver( shared )
    , _impl( new Impl( URI( uri ), DEFAULT_SESSION,
                                     getZMQContext( )))
{
    ZEQWARN << "zeq::Subscriber( const servus::URI&, Receiver& shared ) is "
               "deprecated" << std::endl;
}

Subscriber::~Subscriber()
{
}

bool Subscriber::registerHandler( const uint128_t& event, const EventFunc& func)
{
    return _impl->registerHandler( event, func );
}

bool Subscriber::deregisterHandler( const uint128_t& event )
{
    return _impl->deregisterHandler( event );
}

bool Subscriber::hasHandler( const uint128_t& event ) const
{
    return _impl->hasHandler( event );
}

bool Subscriber::subscribe( servus::Serializable& serializable )
{
    return _impl->subscribe( serializable );
}

bool Subscriber::unsubscribe( const servus::Serializable& serializable )
{
    return _impl->unsubscribe( serializable );
}

const std::string& Subscriber::getSession() const
{
    return _impl->getSession();
}

void Subscriber::addSockets( std::vector< detail::Socket >& entries )
{
    _impl->addSockets( entries );
}

void Subscriber::process( detail::Socket& socket )
{
    _impl->process( socket );
}

void Subscriber::update()
{
    _impl->update( getZMQContext( ));
}

void Subscriber::addConnection( const std::string& uri )
{
    _impl->addConnection( getZMQContext(), uri, uint128_t( ));
}

}
