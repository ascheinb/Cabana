/****************************************************************************
 * Copyright (c) 2018-2022 by the Cabana authors                            *
 * All rights reserved.                                                     *
 *                                                                          *
 * This file is part of the Cabana library. Cabana is distributed under a   *
 * BSD 3-clause license. For the licensing terms see the LICENSE file in    *
 * the top-level directory.                                                 *
 *                                                                          *
 * SPDX-License-Identifier: BSD-3-Clause                                    *
 ****************************************************************************/

/*!
  \file Cabana_Distributor.hpp
  \brief Multi-node particle redistribution
*/
#ifndef CABANA_DISTRIBUTOR_HPP
#define CABANA_DISTRIBUTOR_HPP

#include <Cabana_AoSoA.hpp>
#include <Cabana_CommunicationPlan.hpp>
#include <Cabana_Slice.hpp>

#include <Kokkos_Core.hpp>

#include <mpi.h>

#include <exception>
#include <vector>

namespace Cabana
{
//---------------------------------------------------------------------------//
/*!
  \brief A communication plan for migrating data from one uniquely-owned
  decomposition to another uniquely owned decomposition.

  \tparam DeviceType Device type for which the data for this class will be
  allocated and where parallel compuations will be executed.

  The Distributor allows data to be migrated to an entirely new
  decomposition. Only uniquely-owned decompositions are handled (i.e. each
  local element in the source rank has a single unique destination rank).

  Some nomenclature:

  Export - the data we uniquely own that we will be sending to other ranks.

  Import - the data we uniquely own that we will be receiving from other
  ranks.

  \note We can migrate data to the same rank. In this case a copy will occur
  instead of communication.

  \note To get the number of elements this rank will be receiving from
  migration in the forward communication plan, call totalNumImport() on the
  distributor. This will be needed when in-place migration is not used and a
  user must allocate their own destination data structure.

*/
template <class DeviceType>
class Distributor : public CommunicationPlan<DeviceType>
{
  public:
    /*!
      \brief Topology and export rank constructor. Use this when you already
      know which ranks neighbor each other (i.e. every rank already knows who
      they will be sending and receiving from) as it will be more
      efficient. In this case you already know the topology of the
      point-to-point communication but not how much data to send to and
      receive from the neighbors.

      \tparam ViewType The container type for the export element ranks. This
      container type can be either a Kokkos View or a Cabana Slice.

      \param comm The MPI communicator over which the distributor is defined.

      \param element_export_ranks The destination rank in the target
      decomposition of each locally owned element in the source
      decomposition. Each element will have one unique destination to which it
      will be exported. This export rank may be any one of the listed neighbor
      ranks which can include the calling rank. An export rank of -1 will
      signal that this element is *not* to be exported and will be ignored in
      the data migration. The input is expected to be a Kokkos view or Cabana
      slice in the same memory space as the distributor.

      \param neighbor_ranks List of ranks this rank will send to and receive
      from. This list can include the calling rank. This is effectively a
      description of the topology of the point-to-point communication
      plan. The elements in this list must be unique.

      \note For elements that you do not wish to export, use an export rank of
      -1 to signal that this element is *not* to be exported and will be
      ignored in the data migration. In other words, this element will be
      *completely* removed in the new decomposition. If the data is staying on
      this rank, just use this rank as the export destination and the data
      will be efficiently migrated.
    */
    template <class ViewType>
    Distributor( MPI_Comm comm, const ViewType& element_export_ranks,
                 const std::vector<int>& neighbor_ranks )
        : CommunicationPlan<DeviceType>( comm )
    {
        auto neighbor_ids = this->createFromExportsAndTopology(
            element_export_ranks, neighbor_ranks );
        this->createExportSteering( neighbor_ids, element_export_ranks );
    }

    /*!
      \brief Export rank constructor. Use this when you don't know who you
      will be receiving from - only who you are sending to. This is less
      efficient than if we already knew who our neighbors were because we have
      to determine the topology of the point-to-point communication first.

      \tparam ViewType The container type for the export element ranks. This
      container type can be either a Kokkos View or a Cabana Slice.

      \param comm The MPI communicator over which the distributor is defined.

      \param element_export_ranks The destination rank in the target
      decomposition of each locally owned element in the source
      decomposition. Each element will have one unique destination to which it
      will be exported. This export rank may any one of the listed neighbor
      ranks which can include the calling rank. An export rank of -1 will
      signal that this element is *not* to be exported and will be ignored in
      the data migration. The input is expected to be a Kokkos view or Cabana
      slice in the same memory space as the distributor.

      \note For elements that you do not wish to export, use an export rank of
      -1 to signal that this element is *not* to be exported and will be
      ignored in the data migration. In other words, this element will be
      *completely* removed in the new decomposition. If the data is staying on
      this rank, just use this rank as the export destination and the data
      will be efficiently migrated.
    */
    template <class ViewType>
    Distributor( MPI_Comm comm, const ViewType& element_export_ranks )
        : CommunicationPlan<DeviceType>( comm )
    {
        auto neighbor_ids = this->createFromExportsOnly( element_export_ranks );
        this->createExportSteering( neighbor_ids, element_export_ranks );
    }
};

template <class DeviceType>
class DistributorSorted : public Distributor<DeviceType>
{
    template <class ViewType>
    DistributorSorted( MPI_Comm comm, const ViewType& element_export_ranks )
        : CommunicationPlan<DeviceType>( comm )
    {
        // Bin data based on keys and set up distributor
        bin_data = this->createFromExportsOnlySorted( element_export_ranks );
    }

  protected:
    BinningData<DeviceType> bin_data;
};

//---------------------------------------------------------------------------//
//! \cond Impl
template <typename>
struct is_distributor_impl : public std::false_type
{
};

template <typename DeviceType>
struct is_distributor_impl<Distributor<DeviceType>> : public std::true_type
{
};
//! \endcond

//! Distributor static type checker.
template <class T>
struct is_distributor
    : public is_distributor_impl<typename std::remove_cv<T>::type>::type
{
};

//---------------------------------------------------------------------------//
namespace Impl
{
//! \cond Impl
//---------------------------------------------------------------------------//
// Synchronously move data between a source and destination AoSoA by executing
// the forward communication plan.
template <class Distributor_t, class AoSoA_t>
void distributeData(
    const Distributor_t& distributor, const AoSoA_t& src, AoSoA_t& dst,
    typename std::enable_if<( is_distributor<Distributor_t>::value &&
                              is_aosoa<AoSoA_t>::value ),
                            int>::type* = 0 )
{
    Kokkos::Profiling::pushRegion( "Cabana::migrate" );

    // Get the MPI rank we are currently on.
    int my_rank = -1;
    MPI_Comm_rank( distributor.comm(), &my_rank );

    // Get the number of neighbors.
    int num_n = distributor.numNeighbor();

    // Calculate the number of elements that are staying on this rank and
    // therefore can be directly copied. If any of the neighbor ranks are this
    // rank it will be stored in first position (i.e. the first neighbor in
    // the local list is always yourself if you are sending to yourself).
    std::size_t num_stay =
        ( num_n > 0 && distributor.neighborRank( 0 ) == my_rank )
            ? distributor.numExport( 0 )
            : 0;

    // Allocate a send buffer.
    std::size_t num_send = distributor.totalNumExport() - num_stay;
    Kokkos::View<typename AoSoA_t::tuple_type*,
                 typename Distributor_t::memory_space>
        send_buffer( Kokkos::ViewAllocateWithoutInitializing(
                         "distributor_send_buffer" ),
                     num_send );

    // Allocate a receive buffer.
    Kokkos::View<typename AoSoA_t::tuple_type*,
                 typename Distributor_t::memory_space>
        recv_buffer( Kokkos::ViewAllocateWithoutInitializing(
                         "distributor_recv_buffer" ),
                     distributor.totalNumImport() );

    // Get the steering vector for the sends.
    auto steering = distributor.getExportSteering();

    // Gather the exports from the source AoSoA into the tuple-contiguous send
    // buffer or the receive buffer if the data is staying. We know that the
    // steering vector is ordered such that the data staying on this rank
    // comes first.
    auto build_send_buffer_func = KOKKOS_LAMBDA( const std::size_t i )
    {
        auto tpl = src.getTuple( steering( i ) );
        if ( i < num_stay )
            recv_buffer( i ) = tpl;
        else
            send_buffer( i - num_stay ) = tpl;
    };
    Kokkos::RangePolicy<typename Distributor_t::execution_space>
        build_send_buffer_policy( 0, distributor.totalNumExport() );
    Kokkos::parallel_for( "Cabana::Impl::distributeData::build_send_buffer",
                          build_send_buffer_policy, build_send_buffer_func );
    Kokkos::fence();

    // The distributor has its own communication space so choose any tag.
    const int mpi_tag = 1234;

    // Post non-blocking receives.
    std::vector<MPI_Request> requests;
    requests.reserve( num_n );
    std::pair<std::size_t, std::size_t> recv_range = { 0, 0 };
    for ( int n = 0; n < num_n; ++n )
    {
        recv_range.second = recv_range.first + distributor.numImport( n );

        if ( ( distributor.numImport( n ) > 0 ) &&
             ( distributor.neighborRank( n ) != my_rank ) )
        {
            auto recv_subview = Kokkos::subview( recv_buffer, recv_range );

            requests.push_back( MPI_Request() );

            MPI_Irecv( recv_subview.data(),
                       recv_subview.size() *
                           sizeof( typename AoSoA_t::tuple_type ),
                       MPI_BYTE, distributor.neighborRank( n ), mpi_tag,
                       distributor.comm(), &( requests.back() ) );
        }

        recv_range.first = recv_range.second;
    }

    // Do blocking sends.
    std::pair<std::size_t, std::size_t> send_range = { 0, 0 };
    for ( int n = 0; n < num_n; ++n )
    {
        if ( ( distributor.numExport( n ) > 0 ) &&
             ( distributor.neighborRank( n ) != my_rank ) )
        {
            send_range.second = send_range.first + distributor.numExport( n );

            auto send_subview = Kokkos::subview( send_buffer, send_range );

            MPI_Send( send_subview.data(),
                      send_subview.size() *
                          sizeof( typename AoSoA_t::tuple_type ),
                      MPI_BYTE, distributor.neighborRank( n ), mpi_tag,
                      distributor.comm() );

            send_range.first = send_range.second;
        }
    }

    // Wait on non-blocking receives.
    std::vector<MPI_Status> status( requests.size() );
    const int ec =
        MPI_Waitall( requests.size(), requests.data(), status.data() );
    if ( MPI_SUCCESS != ec )
        throw std::logic_error( "Failed MPI Communication" );

    // Extract the receive buffer into the destination AoSoA.
    auto extract_recv_buffer_func = KOKKOS_LAMBDA( const std::size_t i )
    {
        dst.setTuple( i, recv_buffer( i ) );
    };
    Kokkos::RangePolicy<typename Distributor_t::execution_space>
        extract_recv_buffer_policy( 0, distributor.totalNumImport() );
    Kokkos::parallel_for( "Cabana::Impl::distributeData::extract_recv_buffer",
                          extract_recv_buffer_policy,
                          extract_recv_buffer_func );
    Kokkos::fence();

    // Barrier before completing to ensure synchronization.
    MPI_Barrier( distributor.comm() );
    Kokkos::Profiling::popRegion();
}

// Synchronously move data between a source and destination AoSoA by executing
// the forward communication plan.
template <class Distributor_t, class AoSoA_t>
void distributeDataSorted(
    const Distributor_t& distributor, AoSoA_t& aosoa,
    typename std::enable_if<( is_distributor<Distributor_t>::value &&
                              is_aosoa<AoSoA_t>::value ),
                            int>::type* = 0 )
{
    Kokkos::Profiling::pushRegion( "Cabana::migrate" );

    // Get the size of this communicator.
    int n_ranks = -1;
    MPI_Comm_size( distributor.comm(), &n_ranks );

    // Get the MPI rank we are currently on.
    int my_rank = -1;
    MPI_Comm_rank( distributor.comm(), &my_rank );

    // Create custom data type. Using MPI_BYTE may risk int overflow and MPI
    // doesnt support long long int.
    MPI_Datatype CABANA_MPI_PTL;
    MPI_Type_contiguous( sizeof( Cabana::Tuple<typename AoSoA_t::data_type> ),
                         MPI_BYTE, &CABANA_MPI_PTL );
    MPI_Type_commit( &CABANA_MPI_PTL );

    // If any of the neighbor ranks are this
    // rank it will be stored in first position
    int num_n = distributor.numNeighbor();
    int n_staying = ( num_n > 0 && distributor.neighborRank( 0 ) == my_rank )
                        ? distributor.numExport( 0 )
                        : 0;
    int n_leaving = aosoa.size() - n_staying;

    // Transpose (in place) all vectors particles where at least one particle in
    // the vector is leaving the rank
    int transpose_offset =
        n_staying / AoSoA_t::vector_length; // Mark first vector to transpose
    int nvecs_to_transpose =
        divide_and_round_up( n_staying + n_leaving, AoSoA_t::vector_length ) -
        transpose_offset; // How many vectors to transpose
    transpose_particles_from_AoSoA_to_AoS( aosoa, transpose_offset,
                                           nvecs_to_transpose );

#ifdef USE_GPU_AWARE_MPI
    using MPIDeviceType = typename AoSoA_t::device_type;
#else
    using MPIDeviceType = Kokkos::HostSpace;
#endif
    using data_type = typename AoSoA_t::data_type;

    // Allocate and fill send buffer
    Kokkos::View<data_type*, typename AoSoA_t::device_type> sendbuf(
        Kokkos::ViewAllocateWithoutInitializing( "sendbuf" ), n_leaving );
    {
        Kokkos::View<data_type*, typename AoSoA_t::device_type,
                     Kokkos::MemoryTraits<Kokkos::Unmanaged>>
            optl( (data_type*)aosoa.data() + n_staying, sendbuf.size() );
        Kokkos::deep_copy( sendbuf, optl );
    }

    // Declare unmanaged recvbuffer within the AoSoA, starting after the staying
    // particles
    int n_arriving = distributor.totalNumImport();
    Kokkos::View<data_type*, MPIDeviceType,
                 Kokkos::MemoryTraits<Kokkos::Unmanaged>>
        recvbuf( (data_type*)aosoa.data() + n_staying, n_arriving );

    // Pass MPI pointer to the buffer
    data_type* sendbuf_ptr = sendbuf.data();

#ifdef USE_GPU_AWARE_MPI
    // Pass MPI device pointers
    data_type* recvbuf_ptr = recvbuf.data();
#else
    // Need to use a host mirror of recv buffer for MPI
    auto recvbuf_s = Kokkos::create_mirror_view( recvbuf );

    // Pass MPI host pointers
    data_type* recvbuf_ptr = recvbuf_s.data();
#endif

    // Choose communication ordering
    std::vector<int> ordered_pids( n_ranks );
    bool use_pairwise_ordering = true;
    if ( use_pairwise_ordering )
    {
        // Match up unique pairs to try to keep communication flowing
        int p = 1;
        while ( p < n_ranks )
            p *= 2; // get next power of 2 above n_ranks
        int step = 0;
        for ( int i = 0; i < p; i++ )
        {
            int pair = i ^ my_rank;
            if ( pair < n_ranks )
            {
                ordered_pids[step] = pair;
                step++;
            }
        }
    }
    else
    {
        // Simply put them in order
        for ( int i = 0; i < n_ranks; i++ )
            ordered_pids[i] = i;
    }

    // Calculate displacements
    std::vector<int> dispExport( n_ranks );
    std::vector<int> dispImport( n_ranks );
    dispExport[0] = 0;
    dispImport[0] = 0;
    for ( int i = 1; i < n_ranks; i++ )
    {
        dispExport[i] = dispExport[i - 1] + distributor.numExport( i - 1 );
        dispImport[i] = dispImport[i - 1] + distributor.numImport( i - 1 );
    }

    // Transfer particles
    std::vector<MPI_Request> rrequests( n_ranks );
    for ( int istep = 0; istep < n_ranks; istep++ )
    {
        int i = ordered_pids[istep];
        if ( distributor.numImport( i ) > 0 )
        {
            MPI_Irecv( recvbuf_ptr + dispImport[i], distributor.numImport( i ),
                       CABANA_MPI_PTL, i, i, distributor.comm(),
                       &( rrequests[i] ) );
        }
    }
    for ( int istep = 0; istep < n_ranks; istep++ )
    {
        int i = ordered_pids[istep];
        if ( distributor.numExport( i ) > 0 )
        {
            MPI_Send( sendbuf_ptr + dispExport[i], distributor.numExport( i ),
                      CABANA_MPI_PTL, i, my_rank, distributor.comm() );
        }
    }
    for ( int istep = 0; istep < n_ranks; istep++ )
    {
        int i = ordered_pids[istep];
        if ( distributor.numImport( i ) > 0 )
        {
            MPI_Wait( &( rrequests[i] ), MPI_STATUS_IGNORE );
        }
    }

#ifndef USE_GPU_AWARE_MPI
    // Copy back to device
    Kokkos::deep_copy( recvbuf, recvbuf_s );
#endif

    // How many vectors to transpose
    nvecs_to_transpose =
        divide_and_round_up( n_staying + n_arriving, AoSoA_t::vector_length ) -
        transpose_offset;
    // Transpose back to AoSoA
    transpose_particles_from_AoS_to_AoSoA( aosoa, transpose_offset,
                                           nvecs_to_transpose );

    Kokkos::Profiling::popRegion();
}

//---------------------------------------------------------------------------//
//! \endcond
} // end namespace Impl

//---------------------------------------------------------------------------//
/*!
  \brief Synchronously migrate data between two different decompositions using
  the distributor forward communication plan. Multiple AoSoA version.

  Migrate moves all data to a new distribution that is uniquely owned - each
  element will only have a single destination rank.

  \tparam Distributor_t Distributor type - must be a distributor.

  \tparam AoSoA_t AoSoA type - must be an AoSoA.

  \param distributor The distributor to use for the migration.

  \param src The AoSoA containing the data to be migrated. Must have the same
  number of elements as the inputs used to construct the distributor.

  \param dst The AoSoA to which the migrated data will be written. Must be the
  same size as the number of imports given by the distributor on this
  rank. Call totalNumImport() on the distributor to get this size value.
*/
template <class Distributor_t, class AoSoA_t>
void migrate( const Distributor_t& distributor, const AoSoA_t& src,
              AoSoA_t& dst,
              typename std::enable_if<( is_distributor<Distributor_t>::value &&
                                        is_aosoa<AoSoA_t>::value ),
                                      int>::type* = 0 )
{
    // Check that src and dst are the right size.
    if ( src.size() != distributor.exportSize() )
        throw std::runtime_error( "Source is the wrong size for migration!" );
    if ( dst.size() != distributor.totalNumImport() )
        throw std::runtime_error(
            "Destination is the wrong size for migration!" );

    // Move the data.
    Impl::distributeData( distributor, src, dst );
}

//---------------------------------------------------------------------------//
/*!
  \brief Synchronously migrate data between two different decompositions using
  the distributor forward communication plan. Single AoSoA version that will
  resize in-place. Note that resizing does not necessarily allocate more
  memory. The AoSoA memory will only increase if not enough has already been
  reserved/allocated for the needed number of elements.

  Migrate moves all data to a new distribution that is uniquely owned - each
  element will only have a single destination rank.

  \tparam Distributor_t Distributor type - must be a distributor.

  \tparam AoSoA_t AoSoA type - must be an AoSoA.

  \param distributor The distributor to use for the migration.

  \param aosoa The AoSoA containing the data to be migrated. Upon input, must
  have the same number of elements as the inputs used to construct the
  destributor. At output, it will be the same size as th enumber of import
  elements on this rank provided by the distributor. Before using this
  function, consider reserving enough memory in the data structure so
  reallocating is not necessary.
*/
template <class Distributor_t, class AoSoA_t>
void migrate( const Distributor_t& distributor, AoSoA_t& aosoa,
              typename std::enable_if<( is_distributor<Distributor_t>::value &&
                                        is_aosoa<AoSoA_t>::value ),
                                      int>::type* = 0 )
{
    // Check that the AoSoA is the right size.
    if ( aosoa.size() != distributor.exportSize() )
        throw std::runtime_error( "AoSoA is the wrong size for migration!" );

    // Determine if the source of destination decomposition has more data on
    // this rank.
    bool dst_is_bigger =
        ( distributor.totalNumImport() > distributor.exportSize() );

    // If the destination decomposition is bigger than the source
    // decomposition resize now so we have enough space to do the operation.
    if ( dst_is_bigger )
        aosoa.resize( distributor.totalNumImport() );

    // Move the data.
    Impl::distributeData( distributor, aosoa, aosoa );

    // If the destination decomposition is smaller than the source
    // decomposition resize after we have moved the data.
    if ( !dst_is_bigger )
        aosoa.resize( distributor.totalNumImport() );
}

/*!
  \brief Synchronously migrate data between two different decompositions using
  the distributor forward communication plan. Single AoSoA version that will
  resize in-place. Note that resizing does not necessarily allocate more
  memory. The AoSoA memory will only increase if not enough has already been
  reserved/allocated for the needed number of elements.

  Migrate moves all data to a new distribution that is uniquely owned - each
  element will only have a single destination rank.

  \tparam Distributor_t Distributor type - must be a distributor.

  \tparam AoSoA_t AoSoA type - must be an AoSoA.

  \param distributor The distributor to use for the migration.

  \param aosoa The AoSoA containing the data to be migrated. Upon input, must
  have the same number of elements as the inputs used to construct the
  destributor. At output, it will be the same size as th enumber of import
  elements on this rank provided by the distributor. Before using this
  function, consider reserving enough memory in the data structure so
  reallocating is not necessary.
*/
template <class DeviceType, class AoSoA_t>
void migrate( const DistributorSorted<DeviceType>& distributor, AoSoA_t& aosoa )
{
    // Determine if the source of destination decomposition has more data on
    // this rank.
    bool dst_is_bigger =
        ( distributor.totalNumImport() > distributor.exportSize() );

    // If the destination decomposition is bigger than the source
    // decomposition resize now so we have enough space to do the operation.
    if ( dst_is_bigger )
        aosoa.resize( distributor.totalNumImport() );

    /* Permute the data */
    Cabana::permute( distributor.bin_data, aosoa );

    // Move the data.
    Impl::distributeDataSorted( distributor, aosoa );

    // If the destination decomposition is smaller than the source
    // decomposition resize after we have moved the data.
    if ( !dst_is_bigger )
        aosoa.resize( distributor.totalNumImport() );
}

//---------------------------------------------------------------------------//
/*!
  \brief Synchronously migrate data between two different decompositions using
  the distributor forward communication plan. Slice version. The user can do
  this in-place with the same slice but they will need to manage the resizing
  themselves as we can't resize slices.

  Migrate moves all data to a new distribution that is uniquely owned - each
  element will only have a single destination rank.

  \tparam Distributor_t Distributor type - must be a distributor.

  \tparam Slice_t Slice type - must be an Slice.

  \param distributor The distributor to use for the migration.

  \param src The slice containing the data to be migrated. Must have the same
  number of elements as the inputs used to construct the destributor.

  \param dst The slice to which the migrated data will be written. Must be the
  same size as the number of imports given by the distributor on this
  rank. Call totalNumImport() on the distributor to get this size value.
*/
template <class Distributor_t, class Slice_t>
void migrate( const Distributor_t& distributor, const Slice_t& src,
              Slice_t& dst,
              typename std::enable_if<( is_distributor<Distributor_t>::value &&
                                        is_slice<Slice_t>::value ),
                                      int>::type* = 0 )
{
    // Check that src and dst are the right size.
    if ( src.size() != distributor.exportSize() )
        throw std::runtime_error( "Source is the wrong size for migration!" );
    if ( dst.size() != distributor.totalNumImport() )
        throw std::runtime_error(
            "Destination is the wrong size for migration!" );

    // Get the number of components in the slices.
    size_t num_comp = 1;
    for ( size_t d = 2; d < src.viewRank(); ++d )
        num_comp *= src.extent( d );

    // Get the raw slice data.
    auto src_data = src.data();
    auto dst_data = dst.data();

    // Get the MPI rank we are currently on.
    int my_rank = -1;
    MPI_Comm_rank( distributor.comm(), &my_rank );

    // Get the number of neighbors.
    int num_n = distributor.numNeighbor();

    // Calculate the number of elements that are staying on this rank and
    // therefore can be directly copied. If any of the neighbor ranks are this
    // rank it will be stored in first position (i.e. the first neighbor in
    // the local list is always yourself if you are sending to yourself).
    std::size_t num_stay =
        ( num_n > 0 && distributor.neighborRank( 0 ) == my_rank )
            ? distributor.numExport( 0 )
            : 0;

    // Allocate a send buffer. Note this one is layout right so the components
    // of each element are consecutive in memory.
    std::size_t num_send = distributor.totalNumExport() - num_stay;
    Kokkos::View<typename Slice_t::value_type**, Kokkos::LayoutRight,
                 typename Distributor_t::memory_space>
        send_buffer( Kokkos::ViewAllocateWithoutInitializing(
                         "distributor_send_buffer" ),
                     num_send, num_comp );

    // Allocate a receive buffer. Note this one is layout right so the
    // components of each element are consecutive in memory.
    Kokkos::View<typename Slice_t::value_type**, Kokkos::LayoutRight,
                 typename Distributor_t::memory_space>
        recv_buffer( Kokkos::ViewAllocateWithoutInitializing(
                         "distributor_recv_buffer" ),
                     distributor.totalNumImport(), num_comp );

    // Get the steering vector for the sends.
    auto steering = distributor.getExportSteering();

    // Gather from the source Slice into the contiguous send buffer or,
    // if it is part of the local copy, put it directly in the destination
    // Slice.
    auto build_send_buffer_func = KOKKOS_LAMBDA( const std::size_t i )
    {
        auto s_src = Slice_t::index_type::s( steering( i ) );
        auto a_src = Slice_t::index_type::a( steering( i ) );
        std::size_t src_offset = s_src * src.stride( 0 ) + a_src;
        if ( i < num_stay )
            for ( std::size_t n = 0; n < num_comp; ++n )
                recv_buffer( i, n ) =
                    src_data[src_offset + n * Slice_t::vector_length];
        else
            for ( std::size_t n = 0; n < num_comp; ++n )
                send_buffer( i - num_stay, n ) =
                    src_data[src_offset + n * Slice_t::vector_length];
    };
    Kokkos::RangePolicy<typename Distributor_t::execution_space>
        build_send_buffer_policy( 0, distributor.totalNumExport() );
    Kokkos::parallel_for( "Cabana::migrate::build_send_buffer",
                          build_send_buffer_policy, build_send_buffer_func );
    Kokkos::fence();

    // The distributor has its own communication space so choose any tag.
    const int mpi_tag = 1234;

    // Post non-blocking receives.
    std::vector<MPI_Request> requests;
    requests.reserve( num_n );
    std::pair<std::size_t, std::size_t> recv_range = { 0, 0 };
    for ( int n = 0; n < num_n; ++n )
    {
        recv_range.second = recv_range.first + distributor.numImport( n );

        if ( ( distributor.numImport( n ) > 0 ) &&
             ( distributor.neighborRank( n ) != my_rank ) )
        {
            auto recv_subview =
                Kokkos::subview( recv_buffer, recv_range, Kokkos::ALL );

            requests.push_back( MPI_Request() );

            MPI_Irecv( recv_subview.data(),
                       recv_subview.size() *
                           sizeof( typename Slice_t::value_type ),
                       MPI_BYTE, distributor.neighborRank( n ), mpi_tag,
                       distributor.comm(), &( requests.back() ) );
        }

        recv_range.first = recv_range.second;
    }

    // Do blocking sends.
    std::pair<std::size_t, std::size_t> send_range = { 0, 0 };
    for ( int n = 0; n < num_n; ++n )
    {
        if ( ( distributor.numExport( n ) > 0 ) &&
             ( distributor.neighborRank( n ) != my_rank ) )
        {
            send_range.second = send_range.first + distributor.numExport( n );

            auto send_subview =
                Kokkos::subview( send_buffer, send_range, Kokkos::ALL );

            MPI_Send( send_subview.data(),
                      send_subview.size() *
                          sizeof( typename Slice_t::value_type ),
                      MPI_BYTE, distributor.neighborRank( n ), mpi_tag,
                      distributor.comm() );

            send_range.first = send_range.second;
        }
    }

    // Wait on non-blocking receives.
    std::vector<MPI_Status> status( requests.size() );
    const int ec =
        MPI_Waitall( requests.size(), requests.data(), status.data() );
    if ( MPI_SUCCESS != ec )
        throw std::logic_error( "Failed MPI Communication" );

    // Extract the data from the receive buffer into the destination Slice.
    auto extract_recv_buffer_func = KOKKOS_LAMBDA( const std::size_t i )
    {
        auto s = Slice_t::index_type::s( i );
        auto a = Slice_t::index_type::a( i );
        std::size_t dst_offset = s * dst.stride( 0 ) + a;
        for ( std::size_t n = 0; n < num_comp; ++n )
            dst_data[dst_offset + n * Slice_t::vector_length] =
                recv_buffer( i, n );
    };
    Kokkos::RangePolicy<typename Distributor_t::execution_space>
        extract_recv_buffer_policy( 0, distributor.totalNumImport() );
    Kokkos::parallel_for( "Cabana::migrate::extract_recv_buffer",
                          extract_recv_buffer_policy,
                          extract_recv_buffer_func );
    Kokkos::fence();

    // Barrier before completing to ensure synchronization.
    MPI_Barrier( distributor.comm() );
}

//---------------------------------------------------------------------------//

} // end namespace Cabana

#endif // end CABANA_DISTRIBUTOR_HPP
