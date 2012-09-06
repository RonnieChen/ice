// **********************************************************************
//
// Copyright (c) 2003-2012 ZeroC, Inc. All rights reserved.
//
// This copy of Ice is licensed to you under the terms described in the
// ICE_LICENSE file included in this distribution.
//
// **********************************************************************

#ifndef ICE_METRICSADMIN_I_H
#define ICE_METRICSADMIN_I_H

#include <Ice/Metrics.h>
#include <Ice/Properties.h>
#include <Ice/PropertiesAdmin.h>
#include <Ice/Initialize.h>

#ifdef _MSC_VER
#  define ICE_CPP11_REGEXP
#endif

#ifdef ICE_CPP11_REGEXP
#  include <regex>
#else
#  include <regex.h>
#endif

#include <list>

namespace IceMX
{

class Updater;
typedef IceUtil::Handle<Updater> UpdaterPtr;

class MetricsHelper;
template<typename T> class MetricsHelperT;

class MetricsMapI : public IceUtil::Shared, private IceUtil::Mutex
{
public:

    class RegExp : public IceUtil::Shared
    {
    public:
        
        RegExp(const std::string&, const std::string&);
        ~RegExp();

        bool match(const MetricsHelper&);

    private:
        const std::string _attribute;
#ifdef ICE_CPP11_REGEXP
        std::regex _regex;
#else
        regex_t _preg;
#endif        
    };
    typedef IceUtil::Handle<RegExp> RegExpPtr;

    class Entry : public Ice::LocalObject, protected IceUtil::Mutex
    {
    public:

        Entry(MetricsMapI* map, const MetricsPtr& object, const std::list<Entry*>::iterator&);

        void destroy();
        void  failed(const std::string& exceptionName);
        MetricsFailures getFailures() const;

        virtual MetricsPtr clone() const;
        const std::string& id() const;
        virtual Entry* getMatching(const std::string&, const MetricsHelper&);

        template<typename T> IceInternal::Handle<T>
        attach(const MetricsHelperT<T>& helper)
        {
            Lock sync(*this);
            ++_object->total;
            ++_object->current;
            IceInternal::Handle<T> obj = IceInternal::Handle<T>::dynamicCast(_object);
            assert(obj);
            helper.initMetrics(obj);
            return obj;
        }

        void detach(Ice::Long lifetime)
        {
            MetricsMapI* map;
            {
                Lock sync(*this);
                _object->totalLifetime += lifetime;
                if(--_object->current > 0)
                {
                    return;
                }
                map = _map;
            }
            if(map)
            {
                map->detached(this);
            }
        }

        bool isDetached() const
        {
            Lock sync(*this);
            return _object->current == 0;
        }

        template<typename Function, typename MetricsType> void
        execute(Function func, const MetricsType& obj)
        {
            Lock sync(*this);
            func(obj);
        }

    protected:

        friend class MetricsMapI;

        MetricsPtr _object;
        MetricsMapI* _map;
        StringIntDict _failures;
        std::list<Entry*>::iterator _detachedPos;
    };
    typedef IceUtil::Handle<Entry> EntryPtr;

    MetricsMapI(const std::string&, const Ice::PropertiesPtr&);
    MetricsMapI(const MetricsMapI&);

    virtual ~MetricsMapI();
    
    MetricsFailuresSeq getFailures();
    MetricsFailures getFailures(const std::string&);
    MetricsMap getMetrics() const;
    EntryPtr getMatching(const MetricsHelper&);

    const Ice::PropertyDict& getMapProperties() const
    {
        return _properties;
    }

    virtual MetricsMapI* clone() const = 0;

protected:

    virtual EntryPtr newEntry(const std::string&) = 0;
    std::list<Entry*> _detachedQueue;

private:

    friend class Entry;
    void detached(Entry*);
    
    const Ice::PropertyDict _properties;
    std::vector<std::string> _groupByAttributes;
    std::vector<std::string> _groupBySeparators;
    const int _retain;
    const std::vector<RegExpPtr> _accept;
    const std::vector<RegExpPtr> _reject;

    std::map<std::string, EntryPtr> _objects;
};
typedef IceUtil::Handle<MetricsMapI> MetricsMapIPtr;

class MetricsMapFactory : public IceUtil::Shared
{
public:

    virtual MetricsMapIPtr create(const std::string&, const Ice::PropertiesPtr&) = 0;
};
typedef IceUtil::Handle<MetricsMapFactory> MetricsMapFactoryPtr;

template<class MetricsType> class MetricsMapT : public MetricsMapI
{
public:

    typedef MetricsType T;
    typedef IceInternal::Handle<MetricsType> TPtr;

    typedef MetricsMap MetricsType::* SubMapMember;

    class EntryT : public MetricsMapI::Entry
    {
    public:

        EntryT(MetricsMapT* map, const TPtr& object, const std::list<Entry*>::iterator& p) : 
            Entry(map, object, p), _map(map)
        {
        }

        virtual Entry*
        getMatching(const std::string& mapName, const MetricsHelper& helper)
        {
            typename std::map<std::string, std::pair<MetricsMapIPtr, SubMapMember> >::iterator p = 
                _subMaps.find(mapName);
            if(p == _subMaps.end())
            {
                std::pair<MetricsMapIPtr, SubMapMember> map = _map->createSubMap(mapName);
                if(map.first)
                {
                    p = _subMaps.insert(make_pair(mapName, map)).first;
                }
            }
            if(p == _subMaps.end())
            {
                return 0;
            }            
            return p->second.first->getMatching(helper).get();
        }

        virtual MetricsPtr
        clone() const
        {
            Lock sync(*this);
            TPtr metrics = TPtr::dynamicCast(_object->ice_clone());
            for(typename std::map<std::string, std::pair<MetricsMapIPtr, SubMapMember> >::const_iterator p =
                    _subMaps.begin(); p != _subMaps.end(); ++p)
            {
                metrics.get()->*p->second.second = p->second.first->getMetrics();
            }
            return metrics;
        }

    private:

        MetricsMapT* _map;
        std::map<std::string, std::pair<MetricsMapIPtr, SubMapMember> > _subMaps;
    };
    typedef IceUtil::Handle<EntryT> EntryTPtr;

    MetricsMapT(const std::string& mapPrefix,
                const Ice::PropertiesPtr& properties,
                const std::map<std::string, std::pair<SubMapMember, MetricsMapFactoryPtr> >& subMaps) : 
        MetricsMapI(mapPrefix, properties)
    {
        typename std::map<std::string, std::pair<SubMapMember, MetricsMapFactoryPtr> >::const_iterator p;
        for(p = subMaps.begin(); p != subMaps.end(); ++p)
        {
            const std::string subMapsPrefix = mapPrefix + "Map.";
            std::string subMapPrefix = subMapsPrefix + p->first;
            if(properties->getPropertiesForPrefix(subMapPrefix).empty())
            {
                if(properties->getPropertiesForPrefix(subMapsPrefix).empty())
                {
                    subMapPrefix = mapPrefix;
                }
                else
                {
                    continue; // This sub-map isn't configured.
                }
            }
            _subMaps.insert(std::make_pair(p->first, 
                                           std::make_pair(p->second.first, 
                                                          p->second.second->create(subMapPrefix, properties))));
        }
    }

    MetricsMapT(const MetricsMapT& other) : MetricsMapI(other)
    {
    }

    std::pair<MetricsMapIPtr, SubMapMember>
    createSubMap(const std::string& subMapName)
    {
        typename std::map<std::string, std::pair<SubMapMember, MetricsMapIPtr> >::const_iterator p =
            _subMaps.find(subMapName);
        if(p != _subMaps.end())
        {
            return std::make_pair(p->second.second->clone(), p->second.first);
        }
        return std::make_pair(MetricsMapIPtr(), static_cast<SubMapMember>(0));
    }

protected:

    virtual EntryPtr newEntry(const std::string& id)
    {
        TPtr t = new T();
        t->id = id;
        t->failures = 0;
        if(_subMaps.empty())
        {
            return new Entry(this, t, _detachedQueue.end());
        }
        else
        {
            return new EntryT(this, t, _detachedQueue.end());
        }
    }

    virtual MetricsMapI* clone() const
    {
        return new MetricsMapT<MetricsType>(*this);
    }

    std::map<std::string, std::pair<SubMapMember, MetricsMapIPtr> > _subMaps;
};

template<class MetricsType> class MetricsMapFactoryT : public MetricsMapFactory
{
public:

    virtual MetricsMapIPtr
    create(const std::string& mapPrefix, const Ice::PropertiesPtr& properties)
    {
        return new MetricsMapT<MetricsType>(mapPrefix, properties, _subMaps);
    }

    template<class SubMapMetricsType> void
    registerSubMap(const std::string& subMap, MetricsMap MetricsType::* member)
    {
        _subMaps[subMap] = make_pair(member, new MetricsMapFactoryT<SubMapMetricsType>());
    }

private:

    std::map<std::string, std::pair<MetricsMap MetricsType::*, MetricsMapFactoryPtr> > _subMaps;
};

class MetricsViewI : public IceUtil::Shared
{
public:
    
    MetricsViewI(const std::string&);

    void update(const Ice::PropertiesPtr&, const std::map<std::string, MetricsMapFactoryPtr>&, std::set<std::string>&);

    MetricsView getMetrics();
    MetricsFailuresSeq getFailures(const std::string&);
    MetricsFailures getFailures(const std::string&, const std::string&);

    std::vector<std::string> getMaps() const;

    MetricsMapIPtr getMap(const std::string&) const;

private:

    const std::string _name;
    std::map<std::string, MetricsMapIPtr> _maps;
};
typedef IceUtil::Handle<MetricsViewI> MetricsViewIPtr;

class MetricsAdminI : public MetricsAdmin, private IceUtil::Mutex
{
public:

    MetricsAdminI(const ::Ice::PropertiesPtr&);

    void addUpdater(const std::string&, const UpdaterPtr&);
    void updateViews();

    template<class MetricsType> void 
    registerMap(const std::string& map)
    {
        _factories[map] = new MetricsMapFactoryT<MetricsType>();
    }

    template<class MemberMetricsType, class MetricsType> void
    registerSubMap(const std::string& map, const std::string& subMap, MetricsMap MetricsType::* member)
    {
        std::map<std::string, MetricsMapFactoryPtr>::const_iterator p = _factories.find(map);
        assert(p != _factories.end());

        MetricsMapFactoryT<MetricsType>* factory = dynamic_cast<MetricsMapFactoryT<MetricsType>*>(p->second.get());
        factory->template registerSubMap<MemberMetricsType>(subMap, member);
    }

    virtual Ice::StringSeq getMetricsViewNames(const ::Ice::Current&);
    virtual MetricsView getMetricsView(const std::string&, const ::Ice::Current&);
    virtual MetricsFailuresSeq getMapMetricsFailures(const std::string&, const std::string&, const ::Ice::Current&);
    virtual MetricsFailures getMetricsFailures(const std::string&, const std::string&, const std::string&,
                                               const ::Ice::Current&);

    std::vector<MetricsMapIPtr> getMaps(const std::string&) const;

private:

    std::map<std::string, MetricsViewIPtr> _views;
    std::map<std::string, UpdaterPtr> _updaters;
    std::map<std::string, MetricsMapFactoryPtr> _factories;

    Ice::PropertiesPtr _properties;
};
typedef IceUtil::Handle<MetricsAdminI> MetricsAdminIPtr;

};

#endif
